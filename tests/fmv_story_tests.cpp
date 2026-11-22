#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "drive/save.h"
#include "fmv_player.h"
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
  check(def.audio_index_for("EN") == 0, "EN maps to audio index 0");
  check(def.audio_index_for("fr") == 1, "fr (case-insensitive) maps to audio index 1");
  check(def.audio_index_for("MX") == 2, "MX maps to audio index 2");
  check(def.audio_index_for("JP") == 0, "JP falls back to EN audio index 0");

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

  // Render FMV quad into ui::List and CPU-rasterize.
  carview::ui::List list;
  player.draw(list, 320.0f, 180.0f, "Welcome to Horizon!", true);
  check(!list.empty(), "FmvPlayer::draw emits UI items");
  std::vector<uint8_t> fb(320u * 180u * 4u, 0);
  carview::ui::rasterize(list.items(), fb, 320, 180);
  int non_black = 0;
  for (size_t i = 0; i < fb.size(); i += 4)
    if (fb[i] > 10 || fb[i + 1] > 10 || fb[i + 2] > 10) ++non_black;
  check(non_black > 5000, "rasterized FMV frame has visible pixels");

  // Advance to completion.
  check(player.update(1.0), "synthetic FMV still active at 1.0s");
  check(!player.update(1.5), "synthetic FMV finishes after duration");
  check(player.finished() && !player.active(), "synthetic FMV reports finished");
}

void test_fmv_real_wmv_if_available() {
  const std::filesystem::path sample =
      "C:/Users/neri/code/horizon1-recomp/.local/game/base/media/ui/videos/splash_intros/Dolby_Corona_Intro.wmv";
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
        "FFmpeg decodes frame from Dolby_Corona_Intro.wmv");
  check(rgba.size() == 320u * 180u * 4u, "decoded WMV frame has expected byte count");
  int lit = 0;
  for (size_t i = 0; i < rgba.size(); i += 4)
    if (rgba[i] > 12 || rgba[i + 1] > 12 || rgba[i + 2] > 12) ++lit;
  check(lit > 1000, "decoded WMV frame is non-blank");
}

void test_story_intro_state_machine() {
  carview::StoryIntro story;
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

}  // namespace

int main() {
  test_fmv_def_and_subtitles();
  test_fmv_synthetic_and_audio_mix();
  test_fmv_real_wmv_if_available();
  test_story_intro_state_machine();
  test_pinyon_boot_switches_and_save();
  if (g_failures > 0) {
    std::fprintf(stderr, "fmv_story_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fmv_story_tests: all checks passed\n");
  return 0;
}
