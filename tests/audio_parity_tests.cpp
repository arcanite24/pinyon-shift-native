// Audio parity checks against FH1 (SPEC-AUDIO-EVENTS, SPEC-ENGINE-SOUND).
// Without arguments: synthetic rule checks (no game data). With --corpus:
// checks over the player's files under SHIFT_GAME_ROOT (exit 77 = skipped).
#include <array>
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
#include "audio/fev.h"
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

audio::FevEnvelope env(uint32_t flags, uint32_t param, std::vector<std::pair<float, float>> pts) {
  audio::FevEnvelope e;
  e.flags = flags;
  e.param_index = param;
  for (auto [x, y] : pts) e.points.push_back({x, y, 1});
  return e;
}

// The FEV evaluator: instance placement and crossfade on the layer's control
// parameter, volume envelopes multiplying, normalised parameter axes.
void test_fev_evaluator() {
  audio::FevEvent ev;
  ev.event_type = 0x08;
  ev.volume = 0.5f;
  ev.params = {{"Damage", 0, 0.0f, 1.0f}, {"Rpm", 0, 0.0f, 8000.0f}};
  audio::FevLayer ly;
  ly.control_param_index = 1;
  audio::FevSoundInstance slow, fast;
  slow.sound_def_index = 1;
  slow.start_pos = 0.0f;
  slow.length = 0.6f;
  slow.fadein_len = -1.0f;
  slow.fadeout_len = 0.5f;  // fades over [0.3, 0.6]
  fast.sound_def_index = 2;
  fast.start_pos = 0.3f;
  fast.length = 0.7f;
  fast.fadein_len = 3.0f / 7.0f;  // fades in over [0.3, 0.6]
  fast.fadeout_len = -1.0f;
  ly.instances = {slow, fast};
  ly.envelopes.push_back(env(audio::kFevEnvVolume | 0x04, 0, {{0.0f, 0.0f}, {1.0f, 1.0f}}));
  ly.envelopes.push_back(env(audio::kFevEnvVolume | 0x04, 1, {{0.0f, 0.5f}, {1.0f, 0.5f}}));
  ly.envelopes.push_back(env(audio::kFevEnvPitch | 0x04, 1, {{0.0f, 0.4f}, {1.0f, 0.6f}}));
  audio::FevEnvelope dsp = env(0x04, 0, {{0.0f, 0.0f}});
  dsp.dsp_name = "FMOD Lowpass";
  ly.envelopes.push_back(dsp);  // DSP-unit envelopes do not touch the level
  ev.layers.push_back(ly);

  std::array<audio::FevInstanceLevel, 4> out{};
  auto eval = [&](float dmg, float rpm) {
    const float raw[2] = {dmg, rpm};
    return audio::evaluate_fev_event(ev, raw, 2, out.data(), int(out.size()));
  };
  CHECK(eval(1.0f, 800.0f) == 2);  // rpm 0.1: slow only
  CHECK(std::fabs(out[0].gain - 0.5f * 1.0f * 0.5f) < 1e-5f);
  CHECK(out[1].gain == 0.0f);
  eval(1.0f, 3600.0f);  // rpm 0.45: halfway through both fades
  CHECK(std::fabs(out[0].gain - 0.125f) < 1e-4f);
  CHECK(std::fabs(out[1].gain - 0.125f) < 1e-4f);
  CHECK(out[0].has_pitch_env && std::fabs(out[0].pitch_env - 0.49f) < 1e-4f);
  eval(1.0f, 8000.0f);  // rpm 1.0: fast only
  CHECK(out[0].gain == 0.0f && std::fabs(out[1].gain - 0.25f) < 1e-5f);
  eval(0.0f, 8000.0f);  // no damage: silent
  CHECK(out[1].gain == 0.0f);
  eval(2.0f, 9000.0f);  // clamped to the parameter range
  CHECK(std::fabs(out[1].gain - 0.25f) < 1e-5f);
}

int run_unit() {
  test_title_snapshot_only_without_matrix();
  test_fev_evaluator();
  return failures;
}

// AUDIO-06: Damage.fev's event, read from the disc, gives the damage level.
void corpus_damage_event(const std::filesystem::path& game) {
  audio::FevProject proj;
  std::string err;
  const auto path = game / "media" / "audio" / "cars" / "Engines" / "Damage" / "Damage.fev";
  CHECK(audio::load_fev(path, proj, &err));
  const audio::FevEvent* ev = nullptr;
  for (const auto& e : proj.events)
    if (e.is_complex() && !e.layers.empty()) ev = &e;
  CHECK(ev != nullptr);
  if (!ev) return;
  const int pd = ev->find_param("Damage"), pr = ev->find_param("NormRPM"), pt = ev->find_param("Throttle");
  CHECK(pd >= 0 && pr >= 0 && pt >= 0);
  if (pd < 0 || pr < 0 || pt < 0) return;
  CHECK(ev->layers.front().instances.size() == 2);
  std::array<audio::FevInstanceLevel, 4> out{};
  auto total = [&](float dmg, float rpm, float thr, float* slow = nullptr, float* fast = nullptr) {
    std::array<float, 8> raw{};
    raw[size_t(pd)] = dmg;
    raw[size_t(pr)] = rpm;
    raw[size_t(pt)] = thr;
    const int n = audio::evaluate_fev_event(*ev, raw.data(), raw.size(), out.data(), int(out.size()));
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += out[size_t(i)].gain;
    if (slow) *slow = n > 0 ? out[0].gain : 0.0f;
    if (fast) *fast = n > 1 ? out[1].gain : 0.0f;
    return sum;
  };
  float s = 0, f = 0;
  CHECK(total(0.0f, 0.8f, 1.0f) == 0.0f);  // undamaged: silent
  const float lo = total(1.0f, 0.1f, 1.0f, &s, &f);
  std::printf("damage event: full damage, rpm 0.1, full throttle: slow %.4f fast %.4f\n", s, f);
  CHECK(s > 0.0f && f == 0.0f);  // DamSlow alone at low rpm
  const float hi = total(1.0f, 0.95f, 1.0f, &s, &f);
  std::printf("damage event: full damage, rpm 0.95, full throttle: slow %.4f fast %.4f\n", s, f);
  CHECK(f > 0.0f && s == 0.0f);  // DamFast alone near redline
  CHECK(total(1.0f, 0.95f, 0.0f) < hi);  // throttle raises it
  CHECK(total(0.3f, 0.1f, 1.0f) < lo);   // so does damage
}

int run_corpus() {
  const char* game = std::getenv("SHIFT_GAME_ROOT");
  if (!game || !*game || !std::filesystem::exists(game)) {
    std::printf("audio parity corpus: skipped (no SHIFT_GAME_ROOT)\n");
    return 77;
  }
  const std::filesystem::path root(game);
  corpus_damage_event(root);
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
