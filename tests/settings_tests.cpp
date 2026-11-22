// The pause menu's settings page: rows read from the file's tokens, left
// and right step a row, key rebinds capture a key, live_settings() reflects
// immediate state, and tokens() keeps the file's other options.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "settings_page.h"

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static bool has(const std::vector<std::string>& v, const std::vector<std::string>& seq) {
  for (size_t i = 0; i + seq.size() <= v.size(); ++i) {
    bool ok = true;
    for (size_t k = 0; k < seq.size(); ++k) ok = ok && v[i + k] == seq[k];
    if (ok) return true;
  }
  return false;
}

int main() {
  carview::SettingsPage page;
  page.load({"--size", "1280x720", "--preset", "low", "--no-traffic", "--difficulty", "hard", "--hour", "18",
             "--language", "FR", "--no-rumble", "--rumble-scale", "0.50", "--controller-layout", "2",
             "--steer-dz-in", "0.10", "--no-camera-effects", "--assist-braking", "2", "--assist-steering", "2",
             "--assist-tcs", "1", "--assist-stm", "0", "--assist-shifting", "2", "--assist-line", "1",
             "--assist-rewind", "0", "--keys", "throttle=I,camera=F1"});
  CHECK(page.value("preset") == "low");
  CHECK(page.value("traffic") == "off");
  CHECK(page.value("difficulty") == "hard");
  CHECK(page.value("radio") == "on");
  CHECK(page.value("camera") == "default");
  CHECK(page.value("camera effects") == "off");
  CHECK(page.value("rumble") == "off");
  CHECK(page.value("rumble scale") == "50%");
  CHECK(page.value("controller layout") == "layout 3");
  CHECK(page.value("steer deadzone in") == "10%");
  CHECK(page.value("braking") == "abs off");
  CHECK(page.value("steering") == "simulation");
  CHECK(page.value("traction control") == "on");
  CHECK(page.value("stability control") == "off");
  CHECK(page.value("shifting") == "manual+clutch");
  CHECK(page.value("driving line") == "braking");
  CHECK(page.value("rewind") == "off");
  CHECK(page.value("key throttle") == "I");
  CHECK(page.value("key camera") == "F1");

  // Live settings reflect all loaded controls and assists immediately.
  {
    const auto ls = page.live_settings();
    CHECK(ls.difficulty == 2);
    CHECK(ls.radio && ls.sound && ls.rubberband);
    CHECK(!ls.controls.camera_effects);
    CHECK(!ls.controls.rumble_enabled);
    CHECK(std::fabs(ls.controls.rumble_scale - 0.50) < 1e-6);
    CHECK(ls.controls.layout_id == 2);
    CHECK(std::fabs(ls.controls.deadzones.steering_inside - 0.10) < 1e-6);
    CHECK(ls.controls.keys.matches(drive::ControlAction::kThrottle, "I"));
    CHECK(ls.controls.keys.matches(drive::ControlAction::kSwitchCamera, "F1"));
    CHECK(ls.controls.assists.braking == 2);
    CHECK(ls.controls.assists.steering == 2);
    CHECK(ls.controls.assists.traction_control);
    CHECK(!ls.controls.assists.stability_control);
    CHECK(ls.controls.assists.shifting == 2);
    CHECK(ls.controls.assists.driving_line == 1);
    CHECK(!ls.controls.assists.rewind);
  }

  // Unchanged: the same options come back.
  auto t = page.tokens();
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));
  CHECK(has(t, {"--preset", "low"}) && has(t, {"--no-traffic"}) && has(t, {"--difficulty", "hard"}));
  CHECK(has(t, {"--no-rumble"}) && has(t, {"--controller-layout", "2"}) && has(t, {"--no-camera-effects"}));

  // Rows: preset, shadows, bloom, difficulty, rubber-banding, traffic, ...
  page.open();
  page.step(1);  // preset low -> high
  CHECK(page.value("preset") == "high");
  page.move(5);  // traffic
  page.step(1);  // off -> on
  CHECK(page.value("traffic") == "on");
  page.move(-2);  // difficulty
  page.step(1);   // hard -> pro
  CHECK(page.value("difficulty") == "pro");
  t = page.tokens();
  CHECK(has(t, {"--preset", "high"}) && !has(t, {"--no-traffic"}) && has(t, {"--difficulty", "pro"}));
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));

  // Rebind row capture and commit.
  page.open();
  page.move(-2);  // one before save line -> last rebind row ("key rewind")
  CHECK(page.on_rebind_row());
  page.begin_rebind();
  CHECK(page.capturing_key());
  CHECK(page.commit_rebind("Backspace"));
  CHECK(!page.capturing_key());
  CHECK(page.value("key rewind") == "Backspace");
  CHECK(page.live_settings().controls.keys.matches(drive::ControlAction::kRewind, "Backspace"));

  // The save line follows the last row and wraps.
  page.open();
  page.move(-1);
  CHECK(page.on_save_line());
  page.move(1);
  CHECK(!page.on_save_line());
  std::string title;
  std::vector<std::string> lines;
  int selected = -1;
  page.fill(title, lines, selected);
  CHECK(selected == 0 && lines[0] == "preset  < high >" && lines.back() == "save & apply");

  // Audio Sound Mix (0-200%), master volume/mute, Full Dynamic Range, radio
  // DJ/volume, and Your Music rows round-trip through tokens().
  carview::SettingsPage audio_page;
  audio_page.load({"--master-volume", "75", "--mute", "--full-dynamic-range", "--no-radio-dj", "--radio-volume", "50",
                   "--mix-engine", "175", "--mix-music", "25", "--mix-vo", "200", "--your-music",
                   "--your-music-shuffle", "--your-music-volume", "75"});
  CHECK(audio_page.value("master volume") == "75%");
  CHECK(audio_page.value("mute") == "on");
  CHECK(audio_page.value("full dynamic range") == "on");
  CHECK(audio_page.value("radio dj") == "off");
  CHECK(audio_page.value("radio volume") == "50%");
  CHECK(audio_page.value("mix engine") == "175%");
  CHECK(audio_page.value("mix music") == "25%");
  CHECK(audio_page.value("mix vo") == "200%");
  CHECK(audio_page.value("your music") == "on");
  CHECK(audio_page.value("your music shuffle") == "on");
  CHECK(audio_page.value("your music volume") == "75%");
  auto at = audio_page.tokens();
  CHECK(has(at, {"--master-volume", "75"}) && has(at, {"--mute"}) && has(at, {"--full-dynamic-range"}));
  CHECK(has(at, {"--no-radio-dj"}) && has(at, {"--radio-volume", "50"}));
  CHECK(has(at, {"--mix-engine", "175"}) && has(at, {"--mix-music", "25"}) && has(at, {"--mix-vo", "200"}));
  CHECK(has(at, {"--your-music"}) && has(at, {"--your-music-shuffle"}) && has(at, {"--your-music-volume", "75"}));

  std::printf("settings_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
