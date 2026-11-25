#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "drive/save.h"
#include "fh1data/stringtables.h"
#include "fmv_player.h"
#include "hud.h"
#include "settings.h"
#include "settings_page.h"
#include "story_intro.h"
#include "ui.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void test_fmv_def_and_subtitles() {
  const std::string def_xml = R"(
    <xml>
    <StreamDefinition>
      <Audio id="4" lang="EN"/>
      <Audio id="1" lang="FR"/>
      <Audio id="2" lang="MX"/>
      <Audio id="3" lang="BR"/>
      <Video id="5" />
    </StreamDefinition>
    </xml>
  )";
  carview::FmvStreamDef def;
  check(carview::parse_fmv_def(def_xml, def), "parse_fmv_def succeeds");
  check(def.video_id == 5, "def video id == 5");
  check(def.audio.size() == 4, "def has 4 audio languages");
  check(def.audio_stream_id_for("EN") == 4, "EN maps to retail ASF stream id 4");
  check(def.audio_stream_id_for("fr") == 1, "fr (case-insensitive) maps to retail ASF stream id 1");
  check(def.audio_stream_id_for("MX") == 2, "MX maps to retail ASF stream id 2");
  check(def.audio_stream_id_for("BR") == 3, "BR maps to retail ASF stream id 3");
  check(def.audio_stream_id_for("JP") == 4, "JP falls back to EN retail ASF stream id 4");
  check(def.audio_index_for("EN") == 4 && def.audio_index_for("fr") == 1,
        "audio_index_for preserves retail ASF stream id (not 0-based element index)");

  // Rally / 1000 Club high stream IDs (e.g. id="13").
  const std::string rally_def_xml = R"(
    <StreamDefinition>
      <Audio id="7" lang="DE"/>
      <Audio id="13" lang="EN"/>
      <Video id="14" />
    </StreamDefinition>
  )";
  carview::FmvStreamDef rally_def;
  check(carview::parse_fmv_def(rally_def_xml, rally_def), "parse_fmv_def succeeds for Rally/Club XML");
  check(rally_def.video_id == 14, "Rally def video id == 14");
  check(rally_def.audio_stream_id_for("EN") == 13, "Rally EN maps to ASF stream id 13");
  check(rally_def.audio_stream_id_for("de") == 7, "Rally DE maps to ASF stream id 7");
  check(rally_def.audio_stream_id_for("KR") == 13, "Rally unknown language falls back to EN id 13");

  // 5.1 WMA Pro -> stereo downmix fold check (FL + 0.707*C + 0.707*BL, FR + 0.707*C + 0.707*BR).
  {
    const float in51[6] = {0.4f, 0.2f, 0.2f, 0.9f, 0.1f, 0.3f};
    float out2[2] = {0.0f, 0.0f};
    carview::downmix_interleaved_to_stereo(in51, 6, 1, out2);
    const float want_l = 0.4f + 0.70710678f * 0.2f + 0.70710678f * 0.1f;
    const float want_r = 0.2f + 0.70710678f * 0.2f + 0.70710678f * 0.3f;
    check(std::fabs(out2[0] - want_l) < 1e-4f && std::fabs(out2[1] - want_r) < 1e-4f,
          "5.1 WMA Pro -> stereo downmix uses ITU 0.7071 fold weights and omits LFE");
  }

  const std::string sub_xml = R"(
    <Subtitles>
      <Subtitle0 Start="13.350000"/>
      <Subtitle1 Start="20.000000"/>
      <Subtitle2 Start="22.550000"/>
    </Subtitles>
  )";
  std::vector<carview::FmvSubtitleCue> cues;
  check(carview::parse_fmv_subtitles(sub_xml, cues), "parse_fmv_subtitles succeeds");
  check(cues.size() == 3, "3 subtitle cues parsed");
  check(std::fabs(cues[0].start_s - 13.35) < 1e-4, "first cue start == 13.35");
  check(cues[2].index == 2, "third cue index == 2");
}

void test_fmv_synthetic_and_audio_mix() {
  carview::FmvPlayer player;
  player.open_synthetic(160, 90, 30.0, 2.0, "TestFMV");
  check(player.active(), "synthetic FMV starts active");
  check(player.width() == 160 && player.height() == 90, "synthetic dimensions match");
  check(player.current_frame().size() == 160u * 90u * 4u, "synthetic frame buffer size matches");

  // Mix FMV audio through AudioEngine offline.
  audio::AudioEngine eng(48000);
  auto reader = [](void* ctx, float* out, int frames) -> int {
    return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, frames);
  };
  eng.set_fmv_audio(reader, &player);
  std::vector<float> pcm(480 * 2, 0.0f);
  eng.render_offline(pcm.data(), 480);
  double energy = 0.0;
  for (float v : pcm) energy += double(v) * double(v);
  check(energy > 0.1, "AudioEngine mixes non-silent FMV PCM");
  eng.set_fmv_audio(nullptr, nullptr);

  // Render FMV quad into ui::List and CPU-rasterize (verify fullscreen edge-to-edge without top/bottom black bars).
  carview::ui::List list;
  player.draw(list, 320.0f, 180.0f, "Welcome to Horizon!", true);
  check(!list.empty(), "FmvPlayer::draw emits UI items");
  std::vector<uint8_t> fb(320u * 180u * 4u, 0);
  carview::ui::rasterize(list.items(), fb, 320, 180);
  int non_black = 0;
  for (size_t i = 0; i < fb.size(); i += 4)
    if (fb[i] > 10 || fb[i + 1] > 10 || fb[i + 2] > 10) ++non_black;
  check(non_black > 5000, "rasterized FMV frame has visible pixels");
  // Top and bottom rows on 16:9 must contain live video pixels (no artificial cinema black bars).
  const size_t top_px = (2u * 320u + 160u) * 4u;
  const size_t bot_px = (177u * 320u + 160u) * 4u;
  check((fb[top_px] > 15 || fb[top_px + 1] > 15 || fb[top_px + 2] > 15) &&
            (fb[bot_px] > 15 || fb[bot_px + 1] > 15 || fb[bot_px + 2] > 15),
        "FMV fills 16:9 screen edge-to-edge without top/bottom black bars");

  // On 16:10 (taller than 16:9), FMV cover-fills top-to-bottom without top/bottom black bars.
  carview::ui::List list1610;
  player.draw(list1610, 320.0f, 200.0f, {}, false);
  std::vector<uint8_t> fb1610(320u * 200u * 4u, 0);
  carview::ui::rasterize(list1610.items(), fb1610, 320, 200);
  const size_t top1610 = (2u * 320u + 160u) * 4u;
  const size_t bot1610 = (197u * 320u + 160u) * 4u;
  check((fb1610[top1610] > 15 || fb1610[top1610 + 1] > 15 || fb1610[top1610 + 2] > 15) &&
            (fb1610[bot1610] > 15 || fb1610[bot1610 + 1] > 15 || fb1610[bot1610 + 2] > 15),
        "FMV fills 16:10 screen edge-to-edge without top/bottom black bars");

  // On 3840x2071 (aspect 1.854, scaled 384x207), FMV cover-fills edge-to-edge with ZERO side bars
  // and tags backdrop/video with kUiFlagFullscreen/kUiFlagFullscreenCover so 16:9 HUD lock never exposes 3D edges.
  carview::ui::List list4k;
  player.draw(list4k, 384.0f, 207.0f, {}, false);
  bool saw_fs_bg = false, saw_fs_cover = false;
  for (const auto& it : list4k.items()) {
    if ((it.flags & carview::ui::kUiFlagFullscreen) != 0u) saw_fs_bg = true;
    if ((it.flags & carview::ui::kUiFlagFullscreenCover) != 0u && std::fabs(it.r[0] - (16.0f / 9.0f)) < 0.01f)
      saw_fs_cover = true;
  }
  check(saw_fs_bg && saw_fs_cover, "FmvPlayer::draw tags fullscreen backdrop and cover-fill video quad");
  std::vector<uint8_t> fb4k(384u * 207u * 4u, 0);
  carview::ui::rasterize(list4k.items(), fb4k, 384, 207);
  auto is_lit = [&](uint32_t x, uint32_t y) {
    const size_t idx = (size_t(y) * 384u + size_t(x)) * 4u;
    return fb4k[idx] > 15 || fb4k[idx + 1] > 15 || fb4k[idx + 2] > 15;
  };
  check(is_lit(2, 103) && is_lit(381, 103) && is_lit(192, 2) && is_lit(192, 204),
        "FMV fills 3840x2071 aspect (384x207) edge-to-edge with zero left/right/top/bottom bars");

  // Advance to completion.
  check(player.update(1.0), "synthetic FMV still active at 1.0s");
  check(!player.update(1.5), "synthetic FMV finishes after duration");
  check(player.finished() && !player.active(), "synthetic FMV reports finished");
}

void test_fmv_real_wmv_if_available() {
  std::filesystem::path sample =
      "../horizon1-recomp/.local/game/base/media/ui/videos/splash_intros/Dolby_Corona_Intro.wmv";
  if (const char* gr = std::getenv("SHIFT_GAME_ROOT"); gr && *gr) {
    sample = std::filesystem::path(gr) / "media" / "ui" / "videos" / "splash_intros" / "Dolby_Corona_Intro.wmv";
  }
  std::error_code ec;
  if (!std::filesystem::exists(sample, ec)) return;  // optional on machines without disc
  carview::FmvProbeInfo info;
  if (carview::FmvPlayer::probe(sample, info)) {
    check(info.width == 1280 && info.height == 720, "Dolby_Corona_Intro.wmv probes 1280x720");
    check(info.duration_s > 5.0 && info.has_audio, "Dolby_Corona_Intro.wmv has audio & >5s duration");
  }
  std::vector<uint8_t> rgba;
  std::string err;
  check(carview::FmvPlayer::decode_frame_at(sample, 4.0, 320, 180, rgba, &err),
        "in-process/FFmpeg decodes frame from Dolby_Corona_Intro.wmv");
  check(rgba.size() == 320u * 180u * 4u, "decoded WMV frame has expected byte count");
  int lit = 0;
  for (size_t i = 0; i < rgba.size(); i += 4)
    if (rgba[i] > 12 || rgba[i + 1] > 12 || rgba[i + 2] > 12) ++lit;
  check(lit > 1000, "decoded WMV frame is non-blank");

  // Also test streaming playback (video + audio) of real WMV with no external ffmpeg on Windows.
  carview::FmvPlayer stream;
  check(stream.open(sample, "EN", 640, 360, &err, false), "FmvPlayer::open streams Dolby_Corona_Intro.wmv");
  for (int i = 0; i < 40 && stream.active(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stream.update(0.05, false);
    int non_black = 0;
    const auto& fr = stream.current_frame();
    for (size_t k = 0; k < fr.size(); k += 16)
      if (fr[k] > 8 || fr[k + 1] > 8 || fr[k + 2] > 8) ++non_black;
    if (non_black > 200) break;
  }
  std::vector<float> audio_out(480 * 2, 0.0f);
  int got_audio = 0;
  for (int i = 0; i < 20 && got_audio == 0; ++i) {
    got_audio = stream.read_audio(audio_out.data(), 480);
    if (got_audio == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(got_audio > 0, "FmvPlayer streams real WMV audio frames");
  stream.close();
}

void test_story_intro_state_machine() {
  carview::StoryIntro story;
  // BOOT-01: the boot plays the original's two splashes in its order, never
  // Forza_Intro (the attract movie, BOOT-06) even when the disc has it.
  {
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec) / "shift_boot_splash_test";
    std::filesystem::remove_all(root, ec);
    const auto si = root / "media" / "ui" / "videos" / "splash_intros";
    std::filesystem::create_directories(si, ec);
    for (const char* f : {"T10_MS_Combined.wmv", "Dolby_Corona_Intro.wmv", "Forza_Intro.wmv", "Forza_Tone.wmv",
                          "MSGS_Logo.wmv"})
      std::ofstream(si / f) << "stub";
    carview::StoryIntro boot;
    boot.begin_boot(root, false, false, false);
    check(boot.stage() == carview::StoryStage::kBootSplash, "boot with splashes on disc enters the splash stage");
    check((boot.splash_queue() ==
           std::vector<carview::FmvMovieId>{carview::FmvMovieId::kT10MsCombined, carview::FmvMovieId::kDolbyCorona}),
          "boot splashes are T10_MS_Combined then Dolby_Corona_Intro only");
    check(carview::kAttractMovie == carview::FmvMovieId::kForzaIntro, "attract mode plays Forza_Intro");
    check(!carview::FmvPlayer::resolve_movie(root, carview::kAttractMovie).empty(),
          "the attract movie resolves from splash_intros");
    std::filesystem::remove_all(root, ec);
  }
  // Boot with skip_intro_movies=true, start_in_single_player=false -> title screen.
  {
    auto act = story.begin_boot({}, true, false, false);
    check(act.enter_title_screen, "skip_intro_movies + !start_sp enters title screen");
    check(!story.active(), "story inactive at title screen");
  }
  // Boot with skip_intro_movies=true, start_in_single_player=true, new career -> Viper drive.
  {
    auto act = story.begin_boot({}, true, true, true);
    check(story.stage() == carview::StoryStage::kViperDrive, "start_sp + new career enters Viper drive");
    check(act.swap_car && act.car_media == carview::StoryIntro::kViperMedia, "swaps to SRT Viper GTS");
    check(act.set_gps_target, "sets GPS target to Horizon Festival gate");
  }
  // Update Viper drive: first beat triggers Scott/Alice radio immediate dialogue.
  {
    auto act = story.update(1.0, {1200.0, 600.0}, false, false);
    check(act.radio_immediate_event == "AliceOpeningInterview1", "fires AliceOpeningInterview1 radio beat");
    check(!story.active_caption().empty(), "sets active VO caption");
    carview::ui::List l;
    story.draw_hud(l, 1280.0f, 720.0f);
    check(!l.empty(), "StoryIntro::draw_hud draws Viper objective & caption");
  }
  // Arrive at Horizon Festival gate -> transitions to Corrado qualification race.
  {
    auto act = story.update(0.5, {-1350.0, -200.0}, false, false);
    check(story.stage() == carview::StoryStage::kCorradoQualify, "arrival at gate starts Corrado qualify");
    check(act.swap_car && act.car_media == carview::StoryIntro::kCorradoMedia, "swaps to VW Corrado VR6");
    check(act.launch_qualify_race && act.qualify_route_id == 16, "launches FESTIVAL_FIRST route 16");
  }
  // Finish qualification race -> Alice/Dak/Scott intro beats.
  {
    auto act = story.update(1.0, {-1360.0, -210.0}, false, true);
    check(story.stage() == carview::StoryStage::kFestivalIntroBeats, "race finish enters Festival intro beats");
    (void)act;
  }
  // Skip beats -> Autoshow hand-off & completion.
  {
    auto act = story.skip_stage();
    check(act.open_autoshow && act.grant_starter && act.intro_finished, "skipping beats hands off to Autoshow");
    check(story.completed() && !story.active(), "story marked completed");
  }
}

void test_pinyon_boot_switches_and_save() {
  carview::SettingsPage sp;
  sp.load({"--skip-intro-movies", "--start-in-single-player", "--no-skip-vo-with-a", "--language", "JP"});
  const auto ls = sp.live_settings();
  check(ls.skip_intro_movies, "skip_intro_movies parsed");
  check(ls.start_in_single_player, "start_in_single_player parsed");
  check(!ls.skip_vo_with_a, "skip_vo_with_a=false parsed");
  check(ls.language == "JP", "language=JP parsed");
  check(sp.value("language") == "japanese", "SettingsPage displays japanese for JP");
  const auto toks = sp.tokens();
  carview::SettingsPage sp2;
  sp2.load(toks);
  const auto ls2 = sp2.live_settings();
  check(ls2.skip_intro_movies && ls2.start_in_single_player && !ls2.skip_vo_with_a && ls2.language == "JP",
        "Pinyon boot switches & language round-trip through SettingsPage::tokens()");

  drive::DriveSave s;
  s.car = "VW_Corrado_95";
  s.intro_completed = false;
  const std::string txt = drive::save_to_text(s);
  drive::DriveSave s2;
  std::string err;
  check(drive::save_from_text(txt, s2, &err), "DriveSave round-trips");
  check(!s2.intro_completed, "intro_completed=false preserved in DriveSave");
}

void test_menu_and_settings_pad_navigation() {
  // 1. SettingsPage: Up/Down 25+ times on EVERY tab never switches tab, and every item is reachable.
  carview::SettingsPage sp;
  sp.load({});
  sp.open();
  check(sp.active_tab() == carview::SettingsPage::Tab::kDisplay, "Settings opens on Display tab");
  check(sp.selected_in_tab() == 0, "Settings opens on row 0 of Display tab");

  for (int t = 0; t < int(carview::SettingsPage::Tab::kCount); ++t) {
    sp.select_tab(t);
    const auto expected_tab = carview::SettingsPage::Tab(t);
    check(sp.active_tab() == expected_tab, "select_tab activates target tab");
    check(sp.selected_in_tab() == 0, "select_tab resets row selection to 0");

    const int tab_count = sp.tab_row_count();
    check(tab_count >= 2, "every Settings tab has at least 1 setting plus Save & Apply");

    // Pressing Down 25 times (and full cycle through tab_count) never changes active_tab.
    std::set<int> visited_down;
    visited_down.insert(sp.selected());
    const int steps = std::max(25, tab_count + 5);
    for (int i = 0; i < steps; ++i) {
      sp.move(+1);
      check(sp.active_tab() == expected_tab, "Down never jumps to another Settings tab");
      visited_down.insert(sp.selected());
    }
    check(int(visited_down.size()) == tab_count, "Down reaches every item in the current Settings tab");

    // Pressing Up 25 times (and full cycle through tab_count) never changes active_tab.
    std::set<int> visited_up;
    visited_up.insert(sp.selected());
    for (int i = 0; i < steps; ++i) {
      sp.move(-1);
      check(sp.active_tab() == expected_tab, "Up never jumps to another Settings tab");
      visited_up.insert(sp.selected());
    }
    check(int(visited_up.size()) == tab_count, "Up reaches every item in the current Settings tab");

    // Pressing Left/Right on every item in the tab never changes active_tab or selected row.
    sp.select_tab(t);
    for (int i = 0; i < tab_count; ++i) {
      const int row_before = sp.selected();
      sp.step(+1);
      check(sp.active_tab() == expected_tab && sp.selected() == row_before,
            "Right step adjusts value without changing tab or row");
      sp.step(-1);
      check(sp.active_tab() == expected_tab && sp.selected() == row_before,
            "Left step adjusts value without changing tab or row");
      sp.move(+1);
    }
  }

  // 2. LB/RB (switch_tab) switches tabs cleanly and resets selection to row 0.
  sp.open();
  for (int i = 0; i < int(carview::SettingsPage::Tab::kCount) * 2; ++i) {
    sp.move(+3);  // move away from row 0 before switching
    const auto prev_tab = sp.active_tab();
    sp.switch_tab(+1);
    const int expected_next = (int(prev_tab) + 1) % int(carview::SettingsPage::Tab::kCount);
    check(int(sp.active_tab()) == expected_next, "RB switches to next tab cleanly");
    check(sp.selected_in_tab() == 0, "RB resets selection to row 0");
  }
  for (int i = 0; i < int(carview::SettingsPage::Tab::kCount) * 2; ++i) {
    sp.move(+2);
    const auto prev_tab = sp.active_tab();
    sp.switch_tab(-1);
    const int count = int(carview::SettingsPage::Tab::kCount);
    const int expected_prev = ((int(prev_tab) - 1) % count + count) % count;
    check(int(sp.active_tab()) == expected_prev, "LB switches to previous tab cleanly");
    check(sp.selected_in_tab() == 0, "LB resets selection to row 0");
  }

  // 3. PauseMenu: Down 25 times and Up 25 times visit every item cleanly in Free Roam and In-Race.
  for (bool racing : {false, true}) {
    carview::PauseMenu pm;
    pm.show(racing);
    const int n = int(pm.items().size());
    check(n > 0 && pm.selected == 0, "PauseMenu opens on item 0");
    std::set<int> visited_down, visited_up;
    visited_down.insert(pm.selected);
    for (int i = 0; i < 25; ++i) {
      pm.move(+1);
      check(pm.selected >= 0 && pm.selected < n, "PauseMenu Down stays in valid range");
      visited_down.insert(pm.selected);
    }
    check(int(visited_down.size()) == n, "PauseMenu Down reaches every item");
    visited_up.insert(pm.selected);
    for (int i = 0; i < 25; ++i) {
      pm.move(-1);
      check(pm.selected >= 0 && pm.selected < n, "PauseMenu Up stays in valid range");
      visited_up.insert(pm.selected);
    }
    check(int(visited_up.size()) == n, "PauseMenu Up reaches every item");
  }

  // 4. MenuStickNav: deadzone, no diagonal bleed, no double-fire, clean repeat, latch on open.
  {
    carview::MenuStickNav nav;
    // Inside deadzone -> kNone.
    check(nav.update(0.20, 0.30, 0.0) == carview::MenuStickNav::Dir::kNone, "stick inside deadzone produces kNone");
    // Ambiguous 45-deg diagonal -> kNone.
    check(nav.update(0.70, 0.70, 0.05) == carview::MenuStickNav::Dir::kNone, "45-deg diagonal does not misfire");
    // Clean Down tilt fires once immediately.
    check(nav.update(0.15, 0.85, 0.10) == carview::MenuStickNav::Dir::kDown, "clean Down tilt fires kDown");
    // Next frames before initial_delay_s do NOT double-fire, even if horizontal axis drifts!
    check(nav.update(0.65, 0.75, 0.12) == carview::MenuStickNav::Dir::kNone,
          "Down hold with horizontal drift does not double-fire or leak into Right");
    check(nav.update(0.75, 0.50, 0.30) == carview::MenuStickNav::Dir::kNone,
          "Down hold stays locked vertically before initial delay");
    // After initial delay (0.10 + 0.38 = 0.48), repeats kDown (not kRight!).
    check(nav.update(0.75, 0.50, 0.50) == carview::MenuStickNav::Dir::kDown,
          "Down hold repeats kDown after initial delay without leaking horizontally");
    // Return to neutral clears hold; immediate re-tilt fires without waiting.
    check(nav.update(0.0, 0.10, 0.52) == carview::MenuStickNav::Dir::kNone, "neutral release produces kNone");
    check(nav.update(0.0, 0.80, 0.54) == carview::MenuStickNav::Dir::kDown, "fresh Down tap fires immediately");
    // Latch on menu open ignores already-tilted stick until neutral.
    nav.latch_until_neutral();
    check(nav.update(-0.95, 0.0, 1.00) == carview::MenuStickNav::Dir::kNone, "latched stick ignores pre-held steering");
    check(nav.update(0.0, 0.0, 1.05) == carview::MenuStickNav::Dir::kNone, "neutral clears latch");
    check(nav.update(-0.90, 0.0, 1.07) == carview::MenuStickNav::Dir::kLeft, "Left fires after latch cleared");
  }

  // 5. PT-11 regression: toggling Radio ON/OFF with --tod-preset 19 loaded must NOT change tod_preset_hour,
  //    while stepping Time of Day itself DOES change tod_preset_hour.
  {
    carview::SettingsPage tod_sp;
    tod_sp.load({"--tod-preset", "19"});
    tod_sp.open();
    const double initial_tod = tod_sp.live_settings().tod_preset_hour;
    check(std::fabs(initial_tod - 19.0) < 1e-6, "SettingsPage loads --tod-preset 19");
    tod_sp.select_tab(int(carview::SettingsPage::Tab::kAudio));
    bool toggled_radio = false;
    bool stepped_tod = false;
    const auto rows = tod_sp.display_rows();
    for (const auto& r : rows) {
      if (r.name == "radio") {
        tod_sp.select_row(r.flat_index);
        const bool before_radio = tod_sp.live_settings().radio;
        tod_sp.step(1);
        check(tod_sp.live_settings().radio != before_radio, "stepping radio row toggles radio setting");
        check(std::fabs(tod_sp.live_settings().tod_preset_hour - initial_tod) < 1e-6,
              "toggling radio does not alter tod_preset_hour");
        toggled_radio = true;
      } else if (r.name == "time of day") {
        tod_sp.select_row(r.flat_index);
        tod_sp.step(1);
        check(std::fabs(tod_sp.live_settings().tod_preset_hour - initial_tod) > 1e-6,
              "stepping time of day row updates tod_preset_hour");
        stepped_tod = true;
      }
    }
    check(toggled_radio && stepped_tod, "located both radio and time of day rows in SettingsPage");
  }
}

void test_fmv_subtitles_and_dlc_discovery() {
  // Verify localized Subtitles.str key resolution via fh1::string_hash("Subtitles") + IDS_FMV_01_0.
  const uint32_t expected_packed =
      (uint32_t(fh1::string_hash("Subtitles")) << 16) | uint32_t(fh1::string_hash("IDS_FMV_01_0"));
  const std::string expected_ref = "_&" + std::to_string(expected_packed);
  carview::FmvPlayer::set_subtitle_resolver([&](const std::string& ref) -> std::string {
    if (ref == expected_ref) return "LOCALIZED_FMV_01_LINE_0";
    return ref;
  });
  carview::FmvPlayer p;
  p.open_synthetic(320, 180, 30.0, 5.0, "FMV_01");
  check(p.current_subtitle_text() == "LOCALIZED_FMV_01_LINE_0",
        "FmvPlayer resolves localized Subtitles.str entry via IDS_FMV_01_0 hash");
  carview::FmvPlayer::set_subtitle_resolver(nullptr);

  // Verify DLC movie resolution under state_root (Rally & 1000 Club).
  std::error_code ec;
  const auto tmp = std::filesystem::temp_directory_path(ec) / "shift_fmv_dlc_test";
  std::filesystem::remove_all(tmp, ec);
  const auto rally_dir = tmp / "user" / "0000000000000000" / "4D5309C9" / "00000002" / "RALLY_PKG" / "Media" /
                         "DLCZips" / "1600_pri_65" / "Media" / "UI" / "Videos" / "Splash_Intros";
  const auto club_dir = tmp / "user" / "0000000000000000" / "4D5309C9" / "00000002" / "CLUB_PKG" / "Media" /
                        "DLCZips" / "2700_pri_65" / "Media" / "UI" / "Videos" / "Splash_Intros";
  std::filesystem::create_directories(rally_dir, ec);
  std::filesystem::create_directories(club_dir, ec);
  {
    std::ofstream(rally_dir / "BgLoop.wmv") << "stub";
    std::ofstream(rally_dir / "Forza_Tone_Rally.wmv") << "stub";
    std::ofstream(club_dir / "car_challenges_intro.wmv") << "stub";
  }
  check(!carview::FmvPlayer::resolve_movie({}, carview::FmvMovieId::kRallyBgLoop, tmp).empty(),
        "resolve_movie finds Rally BgLoop.wmv in state_root DLCZips");
  check(!carview::FmvPlayer::resolve_movie({}, carview::FmvMovieId::kRallyForzaTone, tmp).empty(),
        "resolve_movie finds Rally Forza_Tone_Rally.wmv in state_root DLCZips");
  check(!carview::FmvPlayer::resolve_movie({}, carview::FmvMovieId::kClubIntro, tmp).empty(),
        "resolve_movie finds 1000 Club car_challenges_intro.wmv in state_root DLCZips");
  std::filesystem::remove_all(tmp, ec);
}

void test_r3_fmv_subtitles_pll_hold_skip_and_story_units() {
  // R3-UI-14: Explicit End / Duration attributes in subtitle XML honored by current_subtitle_index().
  const std::string sub_xml = R"(
    <Subtitles>
      <Subtitle0 Start="1.0" End="2.5"/>
      <Subtitle1 Start="4.0" Duration="1.5"/>
    </Subtitles>
  )";
  std::vector<carview::FmvSubtitleCue> cues;
  check(carview::parse_fmv_subtitles(sub_xml, cues) && cues.size() == 2, "parse_fmv_subtitles parses End and Duration");
  check(std::fabs(cues[0].end_s - 2.5) < 1e-4, "cue 0 End == 2.5");
  check(std::fabs(cues[1].duration_s - 1.5) < 1e-4, "cue 1 Duration == 1.5");

  carview::FmvPlayer p;
  p.open_synthetic(320, 180, 30.0, 8.0, "SubTest");
  p.set_subtitles(cues);
  p.update(1.5, false);
  check(p.current_subtitle_index() == 0, "cue 0 active inside [1.0, 2.5)");
  p.update(1.5, false);  // t = 3.0 -> past End=2.5
  check(p.current_subtitle_index() == -1, "cue 0 hidden after explicit End=2.5");
  p.update(1.5, false);  // t = 4.5 -> inside [4.0, 5.5)
  check(p.current_subtitle_index() == 1, "cue 1 active inside [4.0, 4.0 + 1.5)");
  p.update(1.5, false);  // t = 6.0 -> past Duration=1.5
  check(p.current_subtitle_index() == -1, "cue 1 hidden after explicit Duration=1.5");

  // R3-UI-14: Multi-line word wrapping keeps every line within max_width_px.
  carview::ui::TextStyle sub_ts{};
  sub_ts.font = carview::ui::Font::kB;
  sub_ts.size = 16.0f;
  sub_ts.color = carview::ui::rgba(255, 255, 255);
  sub_ts.align = carview::ui::Align::kCenter;
  const std::string long_sub =
      "Welcome to Colorado and the Horizon Festival — drive fast, earn wristbands, and chase the sunset across Red Rock.";
  const auto lines = carview::wrap_fmv_subtitle(long_sub, 360.0f, sub_ts);
  check(lines.size() >= 2, "wrap_fmv_subtitle splits long caption into multiple lines");
  for (const auto& ln : lines) {
    check(carview::ui::List::measure(ln, sub_ts) <= 365.0f, "wrapped subtitle line fits within max width");
  }

  // R3-UI-14: +-40 ms audio-clock PLL sync (jitter-free deadband, gentle catch-up / slow-down, monotonic).
  const double within_deadband = carview::fmv_pll_step_clock(2.0, 0.016, 2.040);  // err = +24 ms <= 40 ms
  check(std::fabs(within_deadband - 2.016) < 1e-6, "PLL inside +-40ms deadband steps by exact dt without jitter");
  const double lead_step = carview::fmv_pll_step_clock(2.0, 0.016, 2.136);  // audio leads by +120 ms
  check(lead_step > 2.016 && lead_step < 2.136, "PLL gently catches up when audio leads >40ms");
  const double lag_step = carview::fmv_pll_step_clock(2.0, 0.016, 1.896);   // audio lags by -120 ms
  check(lag_step >= 2.0 && lag_step < 2.016, "PLL gently slows down without reversing when audio lags >40ms");

  // R3-UI-07: 0.75 s hold-to-skip progress ring (stepped in realistic 50 ms frame increments).
  p.reset_hold_skip();
  check(p.hold_skip_progress() == 0.0f, "hold-to-skip starts at 0");
  float p30 = 0.0f;
  for (int i = 0; i < 6; ++i) p30 = p.update_hold_skip(0.05, true);  // 0.30 s total
  check(p30 > 0.35f && p30 < 0.45f, "0.30s hold is ~40% of 0.75s (does not skip on frame 0)");
  float p75 = p30;
  for (int i = 0; i < 9; ++i) p75 = p.update_hold_skip(0.05, true);  // +0.45 s = 0.75 s total
  check(p75 >= 1.0f, "0.75s continuous hold reaches 1.0");
  const float decayed = p.update_hold_skip(0.20, false);
  check(decayed < 1.0f, "releasing skip decays hold progress");

  // R3-UI-07: StoryIntro Viper drive HUD supports Metric and Imperial units cleanly.
  carview::StoryIntro st;
  st.begin_boot({}, true, true, true);
  st.update(0.5, {1200.0, 600.0}, false, false);
  st.set_metric_units(false);
  check(!st.metric_units(), "StoryIntro stores imperial units flag");
  carview::ui::List l_metric, l_imp;
  st.draw_hud(l_metric, 1280.0f, 720.0f, true);
  st.draw_hud(l_imp, 1280.0f, 720.0f, false);
  check(!l_metric.empty() && !l_imp.empty(), "StoryIntro::draw_hud renders both Metric and Imperial HUDs");
}

}  // namespace

int main() {
  test_fmv_def_and_subtitles();
  test_fmv_synthetic_and_audio_mix();
  test_fmv_real_wmv_if_available();
  test_fmv_subtitles_and_dlc_discovery();
  test_r3_fmv_subtitles_pll_hold_skip_and_story_units();
  test_story_intro_state_machine();
  test_pinyon_boot_switches_and_save();
  test_menu_and_settings_pad_navigation();
  if (g_failures > 0) {
    std::fprintf(stderr, "fmv_story_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fmv_story_tests: all checks passed\n");
  return 0;
}
