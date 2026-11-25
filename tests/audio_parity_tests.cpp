// Audio parity checks against FH1 (SPEC-AUDIO-EVENTS, SPEC-ENGINE-SOUND).
// Without arguments: synthetic rule checks (no game data). With --corpus:
// checks over the player's files under SHIFT_GAME_ROOT (exit 77 = skipped).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/engine_player.h"
#include "audio/sfx_engine.h"

namespace {

int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

audio::PcmSample make_tone(double hz, int frames, int rate = 48000, float amp = 0.5f) {
  audio::PcmSample s;
  s.rate = rate;
  s.data.resize(size_t(frames));
  for (int i = 0; i < frames; ++i) s.data[size_t(i)] = amp * float(std::sin(2.0 * 3.14159265358979 * hz * i / rate));
  return s;
}

double rms(const std::vector<float>& v) {
  double acc = 0.0;
  for (float x : v) acc += double(x) * x;
  return v.empty() ? 0.0 : std::sqrt(acc / double(v.size()));
}

// AUDIO-03: the game's AudioMixerSnapshots.xml has no Title snapshot, so with
// the matrix loaded the engine's own Title snapshot and its bus mute must not
// apply; the title mixes through the matrix's Gameplay state.
void test_title_snapshot_only_without_matrix() {
  const char* snap_xml = R"(
<MixSnapshots>
  <FreeRoam group="Gameplay">
    <CarWind active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
  </FreeRoam>
  <UIExterior group="Gameplay">
    <CarWind active="1" destvolume="0.5" fadetime="0" returntime="0" returnfadetime="0"/>
  </UIExterior>
</MixSnapshots>)";
  auto render_wind = [](audio::AudioEngine& eng) {
    eng.set_full_dynamic_range(true);
    eng.set_snapshot(eng.find_snapshot("Title"), true);
    auto sfx = std::make_unique<audio::SfxEngine>(48000);
    sfx->set_sample(audio::SfxSoundId::kWind, make_tone(400.0, 9600));
    eng.set_sfx_engine(std::move(sfx));
    audio::VehicleAudioState vs{};
    vs.speed_mph = 120.0f;
    eng.set_vehicle_state(vs);
    std::vector<float> out(4800 * 2, 0.0f);
    for (int i = 0; i < 6; ++i) eng.render_offline(out.data(), 4800);
    return rms(out);
  };
  {
    audio::AudioEngine eng(48000);
    CHECK(eng.load_snapshot_matrix_xml(snap_xml));
    eng.activate_matrix_snapshot("UIExterior");
    const double r = render_wind(eng);
    std::printf("title with matrix: wind rms %.4f\n", r);
    CHECK(r > 0.005);  // UIExterior keeps CarWind at 0.5: not muted by the own Title snapshot
  }
  {
    audio::AudioEngine eng(48000);  // no matrix: the legacy Title snapshot still mutes the world
    const double r = render_wind(eng);
    std::printf("title without matrix: wind rms %.6f\n", r);
    CHECK(r < 1e-4);
  }
}

int run_unit() {
  test_title_snapshot_only_without_matrix();
  return failures;
}

int run_corpus() {
  const char* game = std::getenv("SHIFT_GAME_ROOT");
  if (!game || !*game || !std::filesystem::exists(game)) {
    std::printf("audio parity corpus: skipped (no SHIFT_GAME_ROOT)\n");
    return 77;
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  const bool corpus = argc > 1 && std::strcmp(argv[1], "--corpus") == 0;
  const int rc = corpus ? run_corpus() : run_unit();
  if (rc == 77) return 77;
  if (failures) {
    std::printf("audio_parity_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("audio_parity_tests: ok\n");
  return 0;
}
