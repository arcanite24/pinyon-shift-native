// Audio parity checks against FH1 (SPEC-AUDIO-EVENTS, SPEC-ENGINE-SOUND).
// Without arguments: synthetic rule checks (no game data). With --corpus:
// checks over the player's files under SHIFT_GAME_ROOT (exit 77 = skipped).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/event_configs.h"
#include "audio/ui_audio.h"
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

// AUDIO-02 / MENU-12: ui4audio play names resolve through the scene's override
// block to an FEV event, and every layer of the event plays at its own level
// and stereo placement.
void test_ui_catalog() {
  const char* xml = R"(<ui4audio><defaults>
    <event play="Accept" group="UIInGame/UI/Nav" cue="Accept" />
    <event play="321CountFestival" group="UIInGame/UI/HorizonCountdowns" cue="321CountFestival" />
    <event play="Stems" group="UIInGame/UI/HorizonCountdowns" cue="Stems" />
    <event play="GoldTrophy" group="UIInGame/UI/Blank" cue="Blank" />
    <event play="NoSample" group="UIInGame/UI/Nav" cue="Missing" />
  </defaults><overrides>
    <override name="Showroom"><event play="Accept" group="UIInGame/UI/Blank" cue="Blank" /></override>
  </overrides><scene_overrides>
    <scene name="075_SHOWROOM_HOMESPACE" override="Showroom" />
  </scene_overrides></ui4audio>)";
  audio::UiAudioMap map;
  CHECK(map.parse(xml));
  audio::FevProject proj;
  proj.dir = "ui";
  for (uint32_t i = 0; i < 5; ++i) {
    audio::FevSoundDef sd;
    audio::FevSoundDefEntry en;
    en.wavebank_name = "UIInGame";
    en.subsound_index = 10 + i;
    sd.entries.push_back(en);
    proj.sound_defs.push_back(sd);
  }
  auto instance = [](uint16_t sd, float vol) {
    audio::FevSoundInstance s;
    s.sound_def_index = sd;
    s.length = 1.0f;
    s.volume = vol;
    s.fadein_len = s.fadeout_len = -1.0f;
    return s;
  };
  {
    audio::FevEvent ev;
    ev.path = "UIInGame/UI/Nav/Accept";
    ev.event_type = 0x10;
    ev.volume = 0.5f;
    ev.simple_instances.push_back(instance(0, 0.8f));
    proj.events.push_back(ev);
  }
  {
    audio::FevEvent ev;  // an LFE stem through a Speaker Level unit plus the main sound
    ev.path = "UIInGame/UI/HorizonCountdowns/321CountFestival";
    ev.event_type = 0x08;
    ev.volume = 0.447f;
    ev.params = {{"param00", 0, 0.0f, 10.0f}};
    audio::FevLayer lfe, main;
    lfe.instances.push_back(instance(1, 1.0f));
    audio::FevEnvelope sl = env(0x04, 0, {{0.0f, 0.0f}});
    sl.dsp_name = "Speaker Level";
    lfe.envelopes.push_back(sl);
    lfe.envelopes.push_back(env(audio::kFevEnvVolume | 0x04, 0, {{0.0f, 0.234f}}));
    main.instances.push_back(instance(2, 1.0f));
    ev.layers = {lfe, main};
    proj.events.push_back(ev);
  }
  {
    audio::FevEvent ev;  // left and right-surround stems of a 5.1 mix
    ev.path = "UIInGame/UI/HorizonCountdowns/Stems";
    ev.event_type = 0x08;
    ev.params = {{"param00", 0, 0.0f, 10.0f}};
    auto stem = [&](uint16_t sd, float lr, float fb) {
      audio::FevLayer ly;
      ly.instances.push_back(instance(sd, 1.0f));
      audio::FevEnvelope root = env(audio::kFevEnvSurroundPan | 0x04, 0, {{0.0f, lr}});
      audio::FevEnvelope front = env(audio::kFevEnvSurroundPan | 0x04, 0, {{0.0f, fb}});
      front.parent_index = 0;
      front.dsp_param_index = 1;
      ly.envelopes = {root, front};
      return ly;
    };
    ev.layers = {stem(3, 1.0f, 1.0f), stem(4, 0.0f, 0.0f)};
    proj.events.push_back(ev);
  }
  audio::UiAudioCatalog cat;
  cat.build(map, {&proj});

  using C = audio::UiAudioCatalog;
  const int accept = cat.resolve("accept");
  CHECK(accept >= 0);
  CHECK(cat.resolve("Accept", "075_showroom_homespace") == C::kSilent);  // the Showroom override
  CHECK(cat.resolve("Accept", "924_POST_RACE") == accept);              // no override for Accept there
  CHECK(cat.resolve("GoldTrophy") == C::kSilent);
  CHECK(cat.resolve("NoSample") == C::kSilent);
  CHECK(cat.resolve("NoSuchName") == C::kUnknown);
  const C::Event* a = cat.event(accept);
  CHECK(a && a->layers.size() == 1);
  if (a && !a->layers.empty()) {
    CHECK(std::fabs(a->layers[0].gain - 0.4f) < 1e-5f);
    CHECK(a->layers[0].subsound == 10 && a->layers[0].bank.filename() == "UIInGame.fsb");
  }
  const C::Event* cnt = cat.event(cat.resolve("321CountFestival"));
  CHECK(cnt && cnt->layers.size() == 2);
  if (cnt && cnt->layers.size() == 2) {
    CHECK(std::fabs(cnt->layers[0].gain - 0.447f * 0.234f) < 1e-5f);  // LFE stem at its envelope level
    CHECK(std::fabs(cnt->layers[0].left - cnt->layers[0].right) < 1e-6f);
    CHECK(std::fabs(cnt->layers[1].gain - 0.447f) < 1e-5f);
  }
  const C::Event* st = cat.event(cat.resolve("Stems"));
  CHECK(st && st->layers.size() == 2);
  if (!st || st->layers.size() != 2) return;
  CHECK(st->layers[0].left > 0.99f && st->layers[0].right < 1e-4f);                         // front left
  CHECK(std::fabs(st->layers[1].right - 0.70710678f) < 1e-4f && st->layers[1].left < 1e-4f);  // rear right
  CHECK(cat.sample_count() == 5);

  // Played through the SFX engine: the left stem reaches only the left channel.
  audio::SfxEngine sfx(48000);
  sfx.set_ui_sample(st->layers[0].sample, make_tone(500.0, 4800));
  std::vector<float> ui(1024 * 2, 0.0f);
  float* buses[audio::kBuses]{};
  buses[int(audio::Bus::kUi)] = ui.data();
  audio::VehicleAudioState vs{};
  sfx.render(vs, buses, 16);  // drains the sample
  std::fill(ui.begin(), ui.end(), 0.0f);
  const auto& l0 = st->layers[0];
  const auto& l1 = st->layers[1];
  sfx.trigger_ui_sample(l0.sample, l0.gain, l0.left, l0.right);
  sfx.trigger_ui_sample(l1.sample, l1.gain, l1.left, l1.right);  // no sample: silent
  CHECK(sfx.is_ui_sample_active(l0.sample));
  CHECK(!sfx.is_ui_sample_active(l1.sample));
  sfx.render(vs, buses, 1024);
  double el = 0, er = 0;
  for (int i = 0; i < 1024; ++i) {
    el += double(ui[size_t(2 * i)]) * ui[size_t(2 * i)];
    er += double(ui[size_t(2 * i + 1)]) * ui[size_t(2 * i + 1)];
  }
  std::printf("ui stem: left energy %.4f right %.6f\n", el, er);
  CHECK(el > 1.0 && er < 1e-9);
}

int run_unit() {
  test_title_snapshot_only_without_matrix();
  test_fev_evaluator();
  test_ui_catalog();
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

// AUDIO-02 / MENU-12: every ui4audio play name the engine can fire exists in
// the disc's ui4audio.xml and plays a sample from UIInGame / General, and the
// SFX engine decodes each of those samples.
void corpus_ui_catalog(const std::filesystem::path& game) {
  audio::UiAudioCatalog cat;
  std::string err;
  CHECK(cat.load(game, &err));
  using C = audio::UiAudioCatalog;
  int playable_events = 0;
  for (const auto& e : cat.events()) playable_events += e.playable ? 1 : 0;
  std::printf("ui4audio: %zu defaults, %zu events (%d playable), %d samples\n", cat.map().defaults.size(),
              cat.events().size(), playable_events, cat.sample_count());
  CHECK(cat.sample_count() <= audio::SfxEngine::kMaxUiSamples);
  const auto names = audio::engine_ui_play_names();
  int ok = 0, blank = 0;
  std::vector<int> fired;
  for (const auto& n : names) {
    const int at = cat.resolve(n);
    const audio::UiAudioEntry* entry = cat.map().resolve(n);
    if (at == C::kUnknown || !entry) {
      std::printf("  MISSING play name %s\n", n.c_str());
      CHECK(at != C::kUnknown);
      continue;
    }
    if (at == C::kSilent) {
      // Only an entry the disc itself leaves silent may be: a Blank entry, or
      // an FEV event that is on the disc but has no sound.
      const int ev = cat.find_event(entry->full_path());
      const bool empty_on_disc = ev >= 0 && cat.events()[size_t(ev)].on_disc && cat.events()[size_t(ev)].layers.empty();
      std::printf("  silent on the disc: %s (%s)\n", n.c_str(), entry->is_blank() ? "Blank" : "empty event");
      CHECK(entry->is_blank() || empty_on_disc);
      ++blank;
      continue;
    }
    ++ok;
    fired.push_back(at);
  }
  std::printf("engine-fired play names: %zu, %d play a sample, %d silent on the disc\n", names.size(), ok, blank);
  CHECK(ok + blank == int(names.size()));
  // The rewind tape stop has no play name; the engine plays its event directly.
  const int stop = cat.find_event("UIInGame/UI/Mulligan/TapeStop");
  CHECK(stop >= 0 && cat.events()[size_t(stop)].playable);
  if (stop >= 0) fired.push_back(stop);
  CHECK(cat.resolve("Accept", "075_SHOWROOM_HOMESPACE") == C::kSilent);

  // The festival pre-race countdown is five stems of one 5.1 mix.
  if (const C::Event* ms = cat.event(cat.resolve("321MomentStartFestival"))) {
    std::printf("321MomentStartFestival: %zu layers\n", ms->layers.size());
    for (const auto& l : ms->layers)
      std::printf("  sub %u gain %.3f L %.3f R %.3f\n", l.subsound, l.gain, l.left, l.right);
    CHECK(ms->layers.size() >= 4);
  } else {
    CHECK(!"321MomentStartFestival");
  }

  // The SFX engine queues and decodes every fired event's layers.
  audio::SfxEngine sfx(48000);
  std::string note;
  CHECK(sfx.load(game, {}, {}, &note));
  std::printf("%s\n", note.c_str());
  CHECK(sfx.ui_catalog() != nullptr);
  std::vector<float> ui(256 * 2, 0.0f);
  float* buses[audio::kBuses]{};
  buses[int(audio::Bus::kUi)] = ui.data();
  audio::VehicleAudioState vs{};
  auto all_fired_loaded = [&]() {
    for (int at : fired)
      for (const auto& l : cat.events()[size_t(at)].layers)
        if (!sfx.has_ui_sample(l.sample)) return false;
    return true;
  };
  const auto t0 = std::chrono::steady_clock::now();
  int last = -1;
  auto last_change = t0;
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(600)) {
    sfx.render(vs, buses, 256);
    const int n = sfx.loaded_ui_sample_count();
    if (n != last) {
      last = n;
      last_change = std::chrono::steady_clock::now();
    }
    if (all_fired_loaded() && std::chrono::steady_clock::now() - last_change > std::chrono::seconds(5)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  int fired_layers = 0, missing = 0;
  for (int at : fired)
    for (const auto& l : cat.events()[size_t(at)].layers) {
      ++fired_layers;
      if (!sfx.has_ui_sample(l.sample)) {
        ++missing;
        std::printf("  undecoded: %s sub %u\n", cat.events()[size_t(at)].path.c_str(), l.subsound);
      }
    }
  std::printf("ui samples decoded: %d of %d; engine-fired layers %d, %d undecoded (%.1f s)\n",
              sfx.loaded_ui_sample_count(), cat.sample_count(), fired_layers, missing,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  CHECK(missing == 0);

  // Accept plays on the UI bus.
  if (const C::Event* acc = cat.event(cat.resolve("Accept"))) {
    for (const auto& l : acc->layers) sfx.trigger_ui_sample(l.sample, l.gain, l.left, l.right);
    double e = 0;
    for (int b = 0; b < 8; ++b) {
      std::fill(ui.begin(), ui.end(), 0.0f);
      sfx.render(vs, buses, 256);
      for (float x : ui) e += double(x) * x;
    }
    std::printf("Accept: %zu layer(s), gain %.3f, energy %.4f\n", acc->layers.size(),
                acc->layers.empty() ? 0.0f : acc->layers[0].gain, e);
    CHECK(e > 1e-3);
  }
}

int run_corpus() {
  const char* game = std::getenv("SHIFT_GAME_ROOT");
  if (!game || !*game || !std::filesystem::exists(game)) {
    std::printf("audio parity corpus: skipped (no SHIFT_GAME_ROOT)\n");
    return 77;
  }
  const std::filesystem::path root(game);
  corpus_damage_event(root);
  corpus_ui_catalog(root);
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
