// Audio parity checks against FH1 (SPEC-AUDIO-EVENTS, SPEC-ENGINE-SOUND).
// Without arguments: synthetic rule checks (no game data). With --corpus:
// checks over the player's files under SHIFT_GAME_ROOT (exit 77 = skipped).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <limits>

#include "audio/audio_engine.h"
#include "audio/bake.h"
#include "audio/event_configs.h"
#include "audio/ui_audio.h"
#include "audio/engine_player.h"
#include "audio/fev.h"
#include "audio/fsb4.h"
#include "audio/sfx_engine.h"
#include "audio/snapshots.h"
#include "audio/sources.h"
#include "cars/database.h"
#include "cars/sources.h"
#include "traffic/config.h"

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
  {
    audio::FevEvent ev;  // a cutscene sting: no ui4audio play name
    ev.path = "UIInGame/UI/Cutscenes/CS_Sting";
    ev.event_type = 0x10;
    ev.simple_instances.push_back(instance(0, 1.0f));
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
  CHECK(cat.sample_count() == 6);
  // The cutscene sting by its trigger's group/event path.
  CHECK(audio::is_cutscene_ui_event("uiingame/ui/cutscenes/CS_Sting") && !audio::is_cutscene_ui_event("UIInGame/UI/Nav/Accept") &&
        !audio::is_cutscene_ui_event("UIInGame/UI/Cutscenes"));
  CHECK(cat.resolve_event("UIInGame/UI/Cutscenes/CS_Sting") >= 0);
  CHECK(cat.resolve("CS_Sting") == C::kUnknown);
  CHECK(cat.resolve_event("UIInGame/UI/Cutscenes/Nope") == C::kUnknown);

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

// AUDIO-01: another car plays its own engine player, placed as a point
// source; once the host owns the other cars' sounds, a car whose sound is not
// ready is silent rather than a shared stand-in loop.
void test_other_car_engines() {
  // The player keeps a reference to its sound: both outlive it here.
  std::vector<std::unique_ptr<audio::EngineSound>> sounds;
  std::vector<std::unique_ptr<audio::PcmSample>> keep;
  auto make_player = [&](double hz) {
    auto es = std::make_unique<audio::EngineSound>();
    es->audio_rpm_idle = 800.0;
    es->audio_rpm_redline = 7000.0;
    audio::Group grp;
    grp.name = "ExhaustL";
    audio::Loop l;
    l.rpm_min = 500, l.rpm_sample = 3000, l.rpm_max = 9000;
    grp.loops = {l};
    es->groups.push_back(grp);
    sounds.push_back(std::move(es));
    keep.push_back(std::make_unique<audio::PcmSample>(make_tone(hz, 4800)));
    auto ep = std::make_unique<audio::EnginePlayer>(
        *sounds.back(), std::vector<std::vector<const audio::PcmSample*>>{{keep.back().get()}}, 48000, 800.0, 7000.0);
    ep->positional = true;
    return ep;
  };
  auto render_other = [](audio::SfxEngine& sfx, const audio::VehicleAudioState& vs, int* zero_crossings = nullptr) {
    std::vector<float> other(1024 * 2, 0.0f);
    float* buses[audio::kBuses]{};
    buses[int(audio::Bus::kOtherCars)] = other.data();
    double e = 0;
    int zc = 0;
    for (int b = 0; b < 12; ++b) {
      std::fill(other.begin(), other.end(), 0.0f);
      sfx.render(vs, buses, 1024);
      if (b < 4) continue;  // settle the ramps
      for (int i = 0; i < 1024; ++i) {
        const float m = other[size_t(2 * i)] + other[size_t(2 * i + 1)];
        e += double(m) * m;
        if (i > 0 && (other[size_t(2 * i - 2)] + other[size_t(2 * i - 1)] < 0.0f) != (m < 0.0f)) ++zc;
      }
    }
    if (zero_crossings) *zero_crossings = zc;
    return e;
  };
  auto car_at = [](uint32_t id, float x) {
    audio::OtherCarAudio c;
    c.active = true;
    c.car_id = id;
    c.pos = {x, 0.0f, 8.0f};
    c.rpm = 3000.0f;
    c.speed_mph = 40.0f;
    c.throttle = 0.6f;
    c.archetype = 2;
    return c;
  };
  audio::VehicleAudioState one{}, two{};
  one.listener_right = two.listener_right = {1.0f, 0.0f, 0.0f};
  one.other_cars[0] = car_at(0x20000001u, -4.0f);
  two.other_cars[0] = car_at(0x20000002u, 4.0f);

  audio::SfxEngine sfx(48000);
  sfx.set_other_car_engine(0x20000001u, make_player(150.0));
  sfx.set_other_car_engine(0x20000002u, make_player(410.0));
  int zc1 = 0, zc2 = 0;
  const double e1 = render_other(sfx, one, &zc1);
  const double e2 = render_other(sfx, two, &zc2);
  std::printf("other cars: engines %d, energy %.3f / %.3f, zero crossings %d / %d\n", sfx.other_car_engine_count(), e1,
              e2, zc1, zc2);
  CHECK(sfx.other_car_engine_count() == 2);
  CHECK(e1 > 1e-3 && e2 > 1e-3);
  CHECK(zc2 > 2 * zc1);  // each car plays its own sound

  // Owned: a car without its player is silent, not a stand-in.
  audio::SfxEngine stand_in(48000);
  for (auto id : {audio::SfxSoundId::kOtherCarLoop, audio::SfxSoundId::kOtherCarLoop2, audio::SfxSoundId::kOtherCarLoop2Hi})
    stand_in.set_sample(id, make_tone(200.0, 4800));
  audio::VehicleAudioState three{};
  three.listener_right = {1.0f, 0.0f, 0.0f};
  three.other_cars[0] = car_at(0x20000003u, 0.0f);
  const double shared = render_other(stand_in, three);
  stand_in.set_other_car_engines_owned(true);
  three.other_cars[0].car_id = 0x20000004u;  // a fresh car
  const double owned = render_other(stand_in, three);
  std::printf("other car without its sound: shared loops %.3f, owned %.6f\n", shared, owned);
  CHECK(shared > 1e-3);
  CHECK(owned < 1e-9);
  // Removing a player hands it back for the control thread to free.
  sfx.set_other_car_engine(0x20000001u, nullptr);
  render_other(sfx, one);
  CHECK(sfx.other_car_engine_count() == 1);
}

// AUDIO-11: the ambience events' parameter names resolve to the engine's
// sources; an unknown one reads the parameter's minimum.
void test_ambience_params() {
  audio::AmbienceParams p;
  p.current_time_s = 3600.0f;
  p.in_race = 1.0f;
  p.car_speed_mph = 42.0f;
  CHECK(audio::AmbienceParams::id_of("(distance)") == audio::AmbienceParams::kDistance);
  CHECK(p.value(audio::AmbienceParams::id_of("CurrentTime")) == 3600.0f);
  CHECK(p.value(audio::AmbienceParams::id_of("inrace")) == 1.0f);
  CHECK(p.value(audio::AmbienceParams::id_of("CarSpeed")) == 42.0f);
  CHECK(audio::AmbienceParams::id_of("PhaseRate") == audio::AmbienceParams::kNone);
  CHECK(std::isnan(p.value(audio::AmbienceParams::kNone)));
  CHECK(audio::ambience_event_hash("Default_Crickets_Bed") == audio::ambience_event_hash("default_crickets_bed"));
}

// Q372 + shift boost direction: the focus engine's 12 kHz top-end rolloff
// trims 12 kHz by ~3 dB while keeping 1 kHz flat, and SfxEngine resolves a
// downshift that updates gear mid-shift to ShiftVolBoostDown instead of Up.
void test_focus_top_rolloff_and_downshift_boost() {
  audio::EngineSound es;
  es.audio_rpm_idle = 800.0;
  es.audio_rpm_redline = 7000.0;
  es.shift_volume_scalar.present = true;
  es.shift_volume_scalar.boost_up_pct = 2.0;
  es.shift_volume_scalar.boost_up_time_s = 0.5;
  es.shift_volume_scalar.boost_down_pct = 0.5;
  es.shift_volume_scalar.boost_down_time_s = 0.5;
  audio::Group grp;
  grp.name = "ExhaustL";
  audio::Loop lp;
  lp.rpm_min = 500;
  lp.rpm_sample = 3000;
  lp.rpm_max = 9000;
  grp.loops = {lp};
  es.groups.push_back(grp);

  auto measure_db = [&](double hz) {
    audio::PcmSample tone = make_tone(hz, 4800, 48000, 0.5f);
    audio::EnginePlayer ep(es, {{&tone}}, 48000, 800.0, 7000.0);
    std::vector<float> buf(4800 * 2, 0.0f);
    for (int b = 0; b < 3; ++b) {
      ep.set({3000.0, 1.0, 1.0, 0.0, audio::View::kFollow});
      ep.render(buf.data(), 4800);
    }
    const double r = rms(buf);
    return 20.0 * std::log10(std::max(1e-12, r));
  };
  const double db_1k = measure_db(1000.0);
  const double db_12k = measure_db(12000.0);
  const double drop_db = db_1k - db_12k;
  std::printf("focus top-end rolloff: 1 kHz %.2f dB, 12 kHz %.2f dB (drop %.2f dB)\n", db_1k, db_12k, drop_db);
  CHECK(drop_db > 2.4 && drop_db < 3.8);

  // Downshift whose gear decrements on the second block of st.shifting triggers downshift boost (0.5).
  audio::PcmSample tone = make_tone(200.0, 4800, 48000, 0.5f);
  audio::EnginePlayer ep(es, {{&tone}}, 48000, 800.0, 7000.0);
  audio::SfxEngine sfx(48000);
  std::vector<float> tr(512 * 2, 0.0f);
  float* buses[audio::kBuses]{};
  buses[int(audio::Bus::kTransmission)] = tr.data();
  audio::VehicleAudioState vs{};
  vs.rpm = 4000.0f;
  vs.idle_rpm = 800.0f;
  vs.redline_rpm = 7000.0f;
  vs.speed_mph = 50.0f;
  vs.gear = 3;
  vs.shifting = false;
  sfx.render(vs, buses, 512, &ep);  // establish prev_gear_ = 3
  vs.shifting = true;              // shift begins before gear updates
  sfx.render(vs, buses, 512, &ep);
  CHECK(std::fabs(ep.shift_factor() - 1.0) < 1e-6);
  vs.gear = 2;                     // gear decrements mid-shift -> downshift
  sfx.render(vs, buses, 512, &ep);
  CHECK(ep.shift_factor() < 0.75);  // boost_down_pct = 0.5
}

// WP-5 audio parity:
// 1. P1-16 CarBusEq: FMOD Ex ParamEQ A = gain_ gives -20.9 dB at Gain=0.30 and -40.0 dB at Gain=0.10.
// 2. P1-16 IMPACT_LFE: chase camera > 5 m behind focus car still fires focus-car LFE, while distant
//    and AIOnlyCollision hits suppress LFE and attenuate 3D impact by -10 dB.
// 3. P2-6 EnginePlayer: non-focus positional cars apply Lowpass distance cutoff cap
//    min(curve_cutoff, max(min_cutoff, dist_cutoff)) without affecting focus cars.
void test_wp5_audio_parity() {
  // 1. CarBusEq exact FMOD Ex ParamEQ attenuation (-20.9 dB @ 0.30, -40.0 dB @ 0.10).
  {
    auto measure_eq_db = [](float gain) {
      audio::CarBusEq eq;
      eq.prepare(48000);
      eq.set_target({false, gain, 3.36f, 3772.0f});
      const int kFrames = 4800;
      std::vector<float> dry(size_t(kFrames) * 2, 0.0f);
      std::vector<float> wet(size_t(kFrames) * 2, 0.0f);
      // Settle slew (0.3 s > 1.0 / 6.0 s).
      for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < kFrames; ++i) {
          const double t = double(b * kFrames + i) / 48000.0;
          const float s = 0.5f * float(std::sin(2.0 * 3.14159265358979323846 * 3772.0 * t));
          dry[size_t(2 * i)] = dry[size_t(2 * i + 1)] = s;
          wet[size_t(2 * i)] = wet[size_t(2 * i + 1)] = s;
        }
        eq.process(wet.data(), kFrames);
      }
      return 20.0 * std::log10(std::max(1e-12, rms(wet)) / std::max(1e-12, rms(dry)));
    };
    const double db_rewind = measure_eq_db(0.30f);
    const double db_countdown = measure_eq_db(0.10f);
    std::printf("CarBusEq: Gain=0.30 -> %.2f dB (expect -20.92), Gain=0.10 -> %.2f dB (expect -40.00)\n", db_rewind,
                db_countdown);
    CHECK(db_rewind < -20.4 && db_rewind > -21.4);
    CHECK(db_countdown < -39.5 && db_countdown > -40.5);
  }

  // 2. IMPACT_LFE focus-car distance (survives FollowHigh chase camera > 5 m) & AIOnlyCollision.
  {
    audio::SfxEngine sfx(48000);
    sfx.set_sample(audio::SfxSoundId::kImpact3d, make_tone(180.0, 4800, 48000, 0.8f));
    sfx.set_sample(audio::SfxSoundId::kImpactLfe, make_tone(50.0, 4800, 48000, 0.8f));
    std::vector<float> col_bus(1024 * 2, 0.0f);
    std::vector<float> lfe_out(1024, 0.0f);
    float* buses[audio::kBuses]{};
    buses[int(audio::Bus::kCollision)] = col_bus.data();

    audio::VehicleAudioState st{};
    st.listener_pos = {0.0f, 2.5f, -8.5f};  // FollowHigh chase camera > 8 m behind focus car
    st.listener_right = {1.0f, 0.0f, 0.0f};
    st.focus_car_pos = {0.0f, 0.0f, 0.0f};
    st.has_focus_car_pos = true;
    sfx.render(st, buses, 64, nullptr, lfe_out.data());  // drain samples

    // Focus-car hit on front bumper (1.8 m from focus car, 10.6 m from chase camera): LFE MUST fire!
    audio::CollisionHit focus_hit{};
    focus_hit.rel_speed_mps = 18.0f;
    focus_hit.pos = {0.0f, 0.5f, 1.8f};
    focus_hit.ai_only = false;
    sfx.trigger_collision(focus_hit, st);
    CHECK(sfx.is_one_shot_active(audio::SfxSoundId::kImpactLfe));
    std::fill(col_bus.begin(), col_bus.end(), 0.0f);
    std::fill(lfe_out.begin(), lfe_out.end(), 0.0f);
    sfx.render(st, buses, 1024, nullptr, lfe_out.data());
    const double focus_3d_rms = rms(col_bus);
    CHECK(rms(lfe_out) > 0.05);

    // Advance past 75 ms collision rate limit and let prior one-shots finish.
    for (int b = 0; b < 10; ++b) sfx.render(st, buses, 1024, nullptr, lfe_out.data());

    // AIOnlyCollision at close range: LFE must stay silent and 3D impact drops ~10 dB.
    audio::CollisionHit ai_hit = focus_hit;
    ai_hit.ai_only = true;
    sfx.trigger_collision(ai_hit, st);
    CHECK(!sfx.is_one_shot_active(audio::SfxSoundId::kImpactLfe));
    std::fill(col_bus.begin(), col_bus.end(), 0.0f);
    std::fill(lfe_out.begin(), lfe_out.end(), 0.0f);
    sfx.render(st, buses, 1024, nullptr, lfe_out.data());
    const double ai_3d_rms = rms(col_bus);
    CHECK(rms(lfe_out) < 1e-6);
    CHECK(ai_3d_rms > 1e-4 && ai_3d_rms < focus_3d_rms * 0.40);
  }

  // 3. Non-focus EnginePlayer Lowpass distance cutoff cap: min(curve_cutoff, max(min_cutoff, dist_cutoff)).
  {
    audio::EngineSound es;
    es.audio_rpm_idle = 800.0;
    es.audio_rpm_redline = 7000.0;
    audio::Group grp;
    grp.name = "ExhaustL";
    grp.lowpass = true;
    grp.lowpass_cutoff.present = true;
    grp.lowpass_cutoff.rpm = 1.0;
    grp.lowpass_cutoff.x = {0.0, 0.5, 1.0};
    grp.lowpass_cutoff.y = {12000.0, 12000.0, 12000.0};
    audio::Loop lp;
    lp.rpm_min = 500;
    lp.rpm_sample = 3000;
    lp.rpm_max = 9000;
    grp.loops = {lp};
    es.groups.push_back(grp);

    audio::PcmSample hi_tone = make_tone(4000.0, 4800, 48000, 0.5f);
    auto render_at = [&](bool positional, double dist_m, double* cutoff_out) {
      audio::EnginePlayer ep(es, {{&hi_tone}}, 48000, 800.0, 7000.0);
      ep.positional = positional;
      std::vector<float> buf(4800 * 2, 0.0f);
      audio::EnginePlayer::Update u{3000.0, 0.8, 0.5, 0.0, positional ? audio::View::kDefault : audio::View::kFollow,
                                    dist_m};
      for (int b = 0; b < 3; ++b) {
        ep.set(u);
        ep.render(buf.data(), 4800);
      }
      if (cutoff_out) *cutoff_out = ep.lowpass_cutoff_hz(0);
      return rms(buf);
    };
    double fc_focus = 0.0, fc_near = 0.0, fc_far = 0.0;
    (void)render_at(false, 120.0, &fc_focus);
    const double r_near = render_at(true, 5.0, &fc_near);
    const double r_far = render_at(true, 110.0, &fc_far);
    CHECK(std::fabs(fc_focus - 12000.0) < 1.0);
    CHECK(std::fabs(fc_near - 12000.0) < 1.0);
    CHECK(fc_far < 4000.0 && fc_far >= 1200.0 - 1e-3);
    CHECK(r_far < r_near * 0.5);

    // When curve_cutoff < min_cutoff, min(curve_cutoff, max(min_cutoff, dist_cutoff)) keeps curve_cutoff (800 Hz).
    es.groups[0].lowpass_cutoff.y = {800.0, 800.0, 800.0};
    double fc_low_curve = 0.0;
    (void)render_at(true, 200.0, &fc_low_curve);
    CHECK(std::fabs(fc_low_curve - 800.0) < 1.0);
  }
}

int run_unit() {
  test_title_snapshot_only_without_matrix();
  test_ambience_params();
  test_fev_evaluator();
  test_ui_catalog();
  test_other_car_engines();
  test_focus_top_rolloff_and_downshift_boost();
  test_wp5_audio_parity();
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

// Q331: Tires.fev's LOD1 skid event and Wind.fev, read from the disc, as the
// engine plays them: wheelspin sounds the PeelOut layer (template pitch -0.05 x
// 4 octaves, envelope 0.4896: 0.82 of the rate, as the recomp measures), a lock
// the HeavyBrake one, a rolling tyre neither; wind is silent below 56.5 mph.
void corpus_tyre_wind_events(const std::filesystem::path& game) {
  audio::FevProject tires;
  CHECK(audio::load_fev(game / "media" / "audio" / "cars" / "Tires" / "Tires.fev", tires, nullptr));
  const audio::FevEvent* skid = tires.find_event("Tires/Tires/LOD1/OnRoad/Skid_RR");
  CHECK(skid != nullptr);
  if (!skid) return;
  const int pa = skid->find_param("SlipAngle"), pr = skid->find_param("SlipRatio"), pl = skid->find_param("Load"),
            prpm = skid->find_param("TireRPM");
  CHECK(pa >= 0 && pr >= 0 && pl >= 0 && prpm >= 0);
  if (pa < 0 || pr < 0 || pl < 0 || prpm < 0) return;
  std::array<audio::FevInstanceLevel, 16> out{};
  auto layer_gain = [&](float ratio, int layer, float* pitch_env = nullptr) {
    std::array<float, 8> raw{};
    raw.fill(std::numeric_limits<float>::quiet_NaN());
    raw[size_t(pa)] = 0.0f;
    raw[size_t(pr)] = ratio;
    raw[size_t(pl)] = 1.0f;
    raw[size_t(prpm)] = 550.0f;
    const int n = audio::evaluate_fev_event(*skid, raw.data(), raw.size(), out.data(), int(out.size()));
    float g = 0.0f;
    for (int i = 0; i < n; ++i)
      if (out[size_t(i)].layer == layer) {
        g += out[size_t(i)].gain;
        if (pitch_env) *pitch_env = out[size_t(i)].pitch_env;
      }
    return g;
  };
  float pe = 0.5f;
  // (Skid_RR's PeelOut instance ends at 0.9998, just short of the clamped maximum.)
  CHECK(layer_gain(4.9f, 4, &pe) > 0.5f);  // PeelOut (HDF) at full spin
  const auto& po_inst = skid->layers[4].instances.front();
  const auto* tpl = tires.template_of(tires.sound_defs[po_inst.sound_def_index]);
  CHECK(tpl != nullptr);
  if (tpl) {
    const float rate = std::exp2(tpl->pitch * audio::kFevPitchOctaves + (pe - 0.5f) * audio::kFevPitchEnvOctaves);
    std::printf("tyre event: PeelOut at full spin plays at %.3f of its rate\n", rate);
    CHECK(rate > 0.80f && rate < 0.84f);
  }
  CHECK(layer_gain(-5.0f, 4) == 0.0f && layer_gain(-5.0f, 2) > 0.5f);  // a lock: HeavyBrake
  CHECK(layer_gain(0.0f, 2) == 0.0f && layer_gain(0.0f, 4) == 0.0f);   // rolling: neither

  audio::FevProject wind;
  CHECK(audio::load_fev(game / "media" / "audio" / "cars" / "Wind" / "Wind.fev", wind, nullptr));
  const audio::FevEvent* w = wind.find_event("Wind/Wind/Wind");
  CHECK(w != nullptr);
  if (!w) return;
  const int ps = w->find_param("Speed");
  CHECK(ps >= 0);
  auto wind_gain = [&](float mph) {
    std::array<float, 8> raw{};
    raw.fill(std::numeric_limits<float>::quiet_NaN());
    raw[size_t(ps)] = mph;
    const int n = audio::evaluate_fev_event(*w, raw.data(), raw.size(), out.data(), int(out.size()));
    return n > 0 ? out[0].gain : 0.0f;
  };
  CHECK(wind_gain(50.0f) == 0.0f);
  CHECK(wind_gain(70.0f) > 0.0f && wind_gain(70.0f) < wind_gain(110.0f));
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
  // The opening's sting and Dak's radio fix: cutscene events ui4audio does not list.
  for (const char* cs : {"UIInGame/UI/Cutscenes/CS_OpeningSequence", "UIInGame/UI/Cutscenes/CS_RadioUnlock"}) {
    const int at = cat.resolve_event(cs);
    std::printf("%s: %d\n", cs, at);
    CHECK(at >= 0);
    if (at >= 0) fired.push_back(at);
  }
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
  // Every LOD1 tyre event binds: 5 surface groups x 4 wheels x Roll/Skid (Q331).
  std::printf("tyre events bound: %d\n", sfx.tyre_event_count());
  CHECK(sfx.tyre_event_count() == 40);
  // Impacts play their Collisions.fev events: the RelativeVelocity position picks
  // the size of hit and its curve the level; the LFE starts above 10 mph (Q334).
  std::printf("impact events bound: %d\n", sfx.impact_event_count());
  CHECK(sfx.impact_event_count() == 35);
  {
    const char* kWall = "Collisions/Collisions/CarAluminum/ConcreteBarrier/IMPACT_3D";
    audio::SfxEngine::ImpactShot shots[8]{};
    float last = 0.0f;
    for (float mph : {10.0f, 40.0f, 80.0f, 150.0f}) {
      const int n = sfx.impact_event_shots(kWall, mph, 5.0f, shots, 8);
      std::printf("wall impact at %.0f mph: %d shot(s)", mph, n);
      for (int i = 0; i < n && i < 8; ++i) std::printf(" [slot %d gain %.3f pitch %.3f]", shots[i].slot, shots[i].gain, shots[i].pitch);
      std::printf("\n");
      CHECK(n == 2);  // one size of hit and one of debris: the sizes do not overlap here
      CHECK(n < 1 || (shots[0].slot >= 0 && shots[0].gain > last));
      if (n >= 1) last = shots[0].gain;
    }
    CHECK(sfx.impact_event_shots(kWall, 40.0f, 60.0f, shots, 8) == 0);  // silent past 40 m
    CHECK(sfx.impact_event_shots("Collisions/Collisions/LFE/IMPACT_LFE", 5.0f, 1.0f, shots, 8) == 0);
    CHECK(sfx.impact_event_shots("Collisions/Collisions/LFE/IMPACT_LFE", 40.0f, 1.0f, shots, 8) == 1);
    CHECK(sfx.impact_event_shots("Collisions/Collisions/LFE/IMPACT_LFE", 40.0f, 1.0f, shots, 8, true) == 0);
  }
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

// AUDIO-01: two real traffic cars decoded from their own banks play through
// the other-car path, differently, and six at once render well inside real
// time.
void corpus_other_car_playback(audio::SoundFiles& files, const traffic::TrafficConfig& cfg,
                               const std::map<int64_t, std::string>& media_by_id,
                               std::map<std::string, int>& groups_by_media) {
  std::vector<std::string> pick;
  for (const auto& m : cfg.models) {
    const auto it = media_by_id.find(m.id);
    if (it != media_by_id.end() && groups_by_media[cars::lower(it->second)] > 0) pick.push_back(it->second);
    if (pick.size() == 2) break;
  }
  CHECK(pick.size() == 2);
  if (pick.size() != 2) return;
  std::vector<std::shared_ptr<audio::BakedSound>> baked;
  for (const auto& m : pick) {
    auto b = std::make_shared<audio::BakedSound>();
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = audio::decode_engine_sound_runtime(files, m, {}, *b, &err);
    size_t loops = 0, decoded = 0;
    for (const auto& g : b->table())
      for (const auto* p : g) {
        ++loops;
        decoded += (p && !p->data.empty()) ? 1 : 0;
      }
    std::printf("decoded %s: %s, %zu groups, %zu of %zu loops (%.1f s)\n", m.c_str(), ok ? "ok" : err.c_str(),
                b->loaded.sound.groups.size(), decoded, loops,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    CHECK(ok && !b->loaded.sound.groups.empty() && decoded > 0);
    if (!ok) return;
    baked.push_back(b);
    // The player alone: as a positional other car at the Default view, and
    // as the focus car at the Follow view (its TrashDSP bands are dB and percent;
    // read as linear factors they silenced the exhaust).
    const auto& s = b->loaded.sound;
    auto alone = [&](bool positional, audio::View view) {
      audio::EnginePlayer ep(s, b->table(), 48000, s.audio_rpm_idle, s.audio_rpm_redline);
      ep.positional = positional;
      audio::EnginePlayer::Update u;
      u.rpm = 0.5 * (s.audio_rpm_idle + s.audio_rpm_redline);
      u.throttle = 0.5;
      u.view = view;
      std::vector<float> st(1024 * 2);
      double e = 0;
      for (int k = 0; k < 8; ++k) {
        ep.set(u);
        ep.render(st.data(), 1024);
        for (float x : st) e += double(x) * x;
      }
      return e;
    };
    const double e_other = alone(true, audio::View::kDefault), e_focus = alone(false, audio::View::kFollow);
    std::printf("  alone: other car %.3f, focus car %.3f (trash dsp %s)\n", e_other, e_focus,
                s.trash_dsp.active ? "on" : "off");
    CHECK(e_other > 1e-3 && e_focus > 1e-3);
  }
  auto player = [&](size_t i) {
    const auto& s = baked[i]->loaded.sound;
    auto ep = std::make_unique<audio::EnginePlayer>(s, baked[i]->table(), 48000, s.audio_rpm_idle, s.audio_rpm_redline);
    ep->master = 0.6;
    ep->positional = true;
    ep->attach_owner(baked[i]);
    return ep;
  };
  audio::SfxEngine sfx(48000);
  std::vector<float> other(512 * 2, 0.0f);
  float* buses[audio::kBuses]{};
  buses[int(audio::Bus::kOtherCars)] = other.data();
  auto run = [&](const audio::VehicleAudioState& vs, int blocks, std::vector<float>* keep_mono) {
    double e = 0;
    for (int b = 0; b < blocks; ++b) {
      std::fill(other.begin(), other.end(), 0.0f);
      sfx.render(vs, buses, 512);
      for (int i = 0; i < 512; ++i) {
        const float m = other[size_t(2 * i)] + other[size_t(2 * i + 1)];
        if (b >= 8) {
          e += double(m) * m;
          if (keep_mono) keep_mono->push_back(m);
        }
      }
    }
    return e;
  };
  std::array<double, 2> energy{};
  std::array<std::vector<float>, 2> mono;
  for (size_t i = 0; i < 2; ++i) {
    const uint32_t id = uint32_t(0x30000001u + i);
    sfx.set_other_car_engine(id, player(i));
    audio::VehicleAudioState vs{};
    vs.listener_right = {1.0f, 0.0f, 0.0f};
    auto& c = vs.other_cars[0];
    c.active = true;
    c.is_traffic = false;  // the car rolloff: an unloaded engine has no traffic rolloff curve
    c.car_id = id;
    c.pos = {0.0f, 0.0f, 10.0f};
    c.rpm = 3000.0f;
    c.throttle = 0.5f;
    energy[i] = run(vs, 40, &mono[i]);
  }
  // Normalised correlation of the two cars' signals: their banks differ.
  double xy = 0, xx = 0, yy = 0;
  for (size_t k = 0; k < std::min(mono[0].size(), mono[1].size()); ++k) {
    xy += double(mono[0][k]) * mono[1][k];
    xx += double(mono[0][k]) * mono[0][k];
    yy += double(mono[1][k]) * mono[1][k];
  }
  const double corr = (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 1.0;
  std::printf("own engines: %s energy %.3f, %s energy %.3f, correlation %.3f\n", pick[0].c_str(), energy[0],
              pick[1].c_str(), energy[1], corr);
  CHECK(energy[0] > 1e-3 && energy[1] > 1e-3);
  CHECK(std::fabs(corr) < 0.9);

  // Six cars at once (the voice count), timed against real time.
  audio::VehicleAudioState six{};
  six.listener_right = {1.0f, 0.0f, 0.0f};
  for (int k = 0; k < audio::VehicleAudioState::kMaxOtherCars; ++k) {
    const uint32_t id = uint32_t(0x30000010u + uint32_t(k));
    sfx.set_other_car_engine(id, player(size_t(k) % 2));
    auto& c = six.other_cars[size_t(k)];
    c.active = true;
    c.car_id = id;
    c.pos = {float(k) * 3.0f - 8.0f, 0.0f, 12.0f};
    c.rpm = 2500.0f + 500.0f * float(k);
    c.throttle = 0.7f;
  }
  run(six, 8, nullptr);
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kBlocks = 94;  // about one second
  run(six, kBlocks, nullptr);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("six own engines: %.1f ms per second of audio (%.1f%% of real time), %d players\n", ms, ms / 10.0,
              sfx.other_car_engine_count());
  CHECK(ms < 250.0);
}

// AUDIO-01: every car the game can put on the road as an opponent or as
// traffic resolves its own engine sound through the same loader the focus
// car uses; only a car with no engine tuning takes the VW_BORAVR6_03 stand-in.
void corpus_other_car_banks(const std::filesystem::path& game, const std::filesystem::path& state) {
  cars::Install install;
  std::string err;
  CHECK(cars::discover_install(game, state, {}, install, &err));
  audio::SoundFiles files;
  CHECK(files.open(install, &err));
  cars::Database db;
  CHECK(cars::load_database(install, db, &err));
  std::map<int64_t, std::string> media_by_id;
  int own = 0, fallback = 0, failed = 0, full = 0, partial = 0, none = 0, mismatched = 0;
  int trash = 0, trash_dead = 0;  // focus-car TrashDSP, and bands that would zero the exhaust
  std::vector<std::string> fallbacks, silent;
  std::set<std::string> seen;
  auto groups_of = [](const audio::EngineSound& s) {
    bool in = false, amb = false, ex = false;
    for (const auto& g : s.groups) {
      if (g.name == "EngineIntake") in = true;
      else if (g.name == "EngineAmbient") amb = true;
      else ex = true;
    }
    return int(in) + int(amb) + int(ex);
  };
  std::map<std::string, int> groups_by_media;
  std::map<std::string, bool> fallback_by_media;
  for (const auto& row : db.cars) {
    media_by_id[row.id] = row.media_name;
    const std::string key = cars::lower(row.media_name);
    if (row.media_name.empty() || !seen.insert(key).second) continue;
    std::string text, e;
    const bool has_et = files.read_tuning(audio::TuningKind::kEngine, row.media_name + "_ET.xml", text, &e);
    audio::LoadedSound ls;
    if (!audio::load_engine_sound(files, row.media_name, ls, &e)) {
      ++failed;
      std::printf("  no engine sound: %s (%s)\n", row.media_name.c_str(), e.c_str());
      continue;
    }
    if (ls.fallback != !has_et) ++mismatched;
    if (ls.fallback) {
      ++fallback;
      fallbacks.push_back(row.media_name);
    } else {
      ++own;
    }
    if (ls.sound.trash_dsp.active) {
      ++trash;
      for (const auto& b : ls.sound.trash_dsp.bands)
        if (b.output_gain < 0.01 || b.input_gain < 0.01) {
          ++trash_dead;
          break;
        }
    }
    const int ng = groups_of(ls.sound);
    groups_by_media[key] = ng;
    fallback_by_media[key] = ls.fallback;
    if (ng == 3) ++full;
    else if (ng > 0) ++partial;
    else {
      ++none;
      silent.push_back(row.media_name);
    }
  }
  std::printf("engine sound over %zu car rows (%zu models): %d own tuning, %d VW_BORAVR6_03 fallback, %d failed\n",
              db.cars.size(), seen.size(), own, fallback, failed);
  std::printf("  groups: %d all three, %d one or two, %d none (spec census: 208 / 30 / 1)\n", full, partial, none);
  for (const auto& m : fallbacks) std::printf("  fallback: %s\n", m.c_str());
  for (const auto& m : silent) std::printf("  no group: %s\n", m.c_str());
  CHECK(failed == 0);
  CHECK(silent.size() == 1 && silent.front() == "NIS_Leaf_11");  // the one electric car
  CHECK(mismatched == 0);  // the stand-in exactly when the car has no engine tuning
  CHECK(own > 200);
  std::printf("  focus TrashDSP on %d tunings, %d with a band that silences the exhaust\n", trash, trash_dead);
  CHECK(trash > 100 && trash_dead == 0);

  // The traffic models: each resolves its own sound.
  traffic::TrafficConfig cfg;
  CHECK(traffic::read_traffic_config(game, cfg, &err));
  int t_own = 0, t_fb = 0, t_missing = 0, t_absent = 0, t_electric = 0;
  for (const auto& m : cfg.models) {
    const auto it = media_by_id.find(m.id);
    const std::string key = it == media_by_id.end() ? std::string{} : cars::lower(it->second);
    const auto g = groups_by_media.find(key);
    if (it == media_by_id.end()) {
      // A model the traffic config lists but the merged database does not
      // have: the game cannot spawn it either.
      std::printf("  traffic model %lld: not in Data_Car\n", (long long)m.id);
      ++t_absent;
      continue;
    }
    if (g != groups_by_media.end() && g->second == 0) {
      // An engine tuning with no group: the electric car, whose sound is the
      // ElectricMotor event project (SPEC-ENGINE-SOUND).
      ++t_electric;
      std::printf("  traffic model %lld (%s): electric motor\n", (long long)m.id, it->second.c_str());
      continue;
    }
    if (g == groups_by_media.end()) {
      ++t_missing;
      std::printf("  traffic model %lld (%s): no engine sound\n", (long long)m.id, it->second.c_str());
      continue;
    }
    (fallback_by_media[key] ? t_fb : t_own) += 1;
  }
  std::printf("traffic models: %zu, %d own engine sound, %d fallback, %d electric, %d without, %d not in Data_Car\n",
              cfg.models.size(), t_own, t_fb, t_electric, t_missing, t_absent);
  CHECK(!cfg.models.empty() && t_missing == 0);
  corpus_other_car_playback(files, cfg, media_by_id, groups_by_media);
}

// AUDIO-11: the disc's ambience events drive the beds and emitters through
// the player's own render path: CurrentTime (crickets day / night), InRace
// (race crowds), PreRaceAmbience (grandstand), CarSpeed (the region quad)
// and (distance) (the event's own rolloff).
void corpus_ambience_events(const std::filesystem::path& game) {
  std::error_code ec;
  const auto cache = std::filesystem::temp_directory_path(ec) / "shift-audio-parity-ambience";
  // The map's placements (ribbon and tiles), as the game loads them.
  audio::WorldAcousticsData world;
  CHECK(world.load_configs(game));
  const int tiles = world.load_soundscapes_from_archive(game / "media" / "tracks" / "colorado" / "bin.zip");
  CHECK(tiles > 1000);
  std::vector<uint32_t> placed;
  for (const auto& em : world.emitters())
    if (em.playable) placed.push_back(em.event_hash);
  std::sort(placed.begin(), placed.end());
  placed.erase(std::unique(placed.begin(), placed.end()), placed.end());

  audio::AmbienceBedPlayer amb(48000);
  const auto t0 = std::chrono::steady_clock::now();
  const int queued = amb.load_from_disc(game, cache);
  CHECK(queued > 0);
  // Only the placed events' loops are decoded.
  const int wanted = amb.want_events(placed);
  CHECK(wanted > 10 && wanted < queued);
  constexpr int kBlock = 512;
  std::vector<float> buf(size_t(kBlock) * 2);
  audio::RtAcousticsSample quiet{};
  const char* snap = "DefaultLocation";
  std::memcpy(quiet.ambience_snapshot.data(), snap, std::strlen(snap));
  audio::AmbienceParams p0{};
  int last = -1;
  auto stable_since = std::chrono::steady_clock::now();
  for (;;) {
    std::fill(buf.begin(), buf.end(), 0.0f);
    amb.render(quiet, p0, buf.data(), kBlock);
    const int have = amb.loaded_bed_count() + amb.loaded_event_sample_count();
    const auto now = std::chrono::steady_clock::now();
    if (have != last) {
      last = have;
      stable_since = now;
    }
    if ((amb.loaded_event_sample_count() >= wanted && now - stable_since > std::chrono::milliseconds(500)) ||
        now - stable_since > std::chrono::seconds(20) || now - t0 > std::chrono::minutes(6))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::printf("ambience: %d events; %d beds and %d of %d event loops decoded (%d wanted by %zu placed events) in "
              "%.1f s\n",
              amb.loaded_event_count(), amb.loaded_bed_count(), amb.loaded_event_sample_count(),
              queued - amb.loaded_bed_count(), wanted, placed.size(),
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  CHECK(amb.loaded_event_count() > 50);
  CHECK(amb.loaded_event_sample_count() == wanted);
  std::printf("  event loops in memory: %.1f MB\n", double(amb.loaded_event_sample_bytes()) / 1e6);
  CHECK(amb.loaded_event_sample_bytes() < size_t(64) << 20);

  auto emitter = [&](const char* event, float distance) {
    audio::RtAcousticsSample s = quiet;
    auto& e = s.active_emitters[0];
    e.active = true;
    e.event_hash = audio::ambience_event_hash(event);
    e.id_hash = e.event_hash;
    e.distance_m = distance;
    e.pan = 0.0f;
    return s;
  };
  // Two seconds to settle the gains, then one second measured.
  auto energy = [&](const audio::RtAcousticsSample& s, const audio::AmbienceParams& p, std::vector<float>* keep) {
    double e = 0;
    for (int b = 0; b < 3 * 48000 / kBlock; ++b) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      amb.render(s, p, buf.data(), kBlock);
      if (b < 2 * 48000 / kBlock) continue;
      for (int i = 0; i < kBlock; ++i) {
        const float m = buf[size_t(2 * i)] + buf[size_t(2 * i + 1)];
        e += double(m) * m;
        if (keep) keep->push_back(m);
      }
    }
    return e;
  };
  auto corr = [](const std::vector<float>& a, const std::vector<float>& b) {
    double xy = 0, xx = 0, yy = 0;
    for (size_t k = 0; k < std::min(a.size(), b.size()); ++k) {
      xy += double(a[k]) * b[k];
      xx += double(a[k]) * a[k];
      yy += double(b[k]) * b[k];
    }
    return (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 1.0;
  };

  // The region bed alone: CarSpeed's curve (1 to 5 mph, 0 by 35 mph).
  audio::AmbienceParams fast = p0;
  fast.car_speed_mph = fast.speed = 60.0f;
  const double bed_still = energy(quiet, p0, nullptr), bed_fast = energy(quiet, fast, nullptr);
  std::printf("  region bed (Default_Quad_Plain): %.4f at 0 mph, %.6f at 60 mph\n", bed_still, bed_fast);
  CHECK(bed_still > 1e-3 && bed_fast < 0.01 * bed_still);

  // The emitters at 40 mph, where the quad bed is out (0 by 35 mph) and the
  // emitters' own CarSpeed curves are not (crickets 0.5, race crowd 0.75).
  audio::AmbienceParams noon = p0;
  noon.car_speed_mph = noon.speed = 40.0f;
  CHECK(energy(quiet, noon, nullptr) < 0.01 * bed_still);

  // Time of day on a placed Default_Crickets_Bed (1,783 on the map):
  // Crickets_Day at noon, Crickets_Night at 23:00.
  const auto crickets = emitter("Default_Crickets_Bed", 10.0f);
  audio::AmbienceParams night = noon;
  noon.current_time_s = 12.0f * 3600.0f;
  night.current_time_s = 23.0f * 3600.0f;
  std::vector<float> s_noon, s_night;
  const double e_noon = energy(crickets, noon, &s_noon), e_night = energy(crickets, night, &s_night);
  const double c_tod = corr(s_noon, s_night);
  audio::AmbienceParams pre = noon;
  pre.pre_race_ambience = 1.0f;
  const double e_pre = energy(crickets, pre, nullptr);
  const double e_far = energy(emitter("Default_Crickets_Bed", 70.0f), noon, nullptr);
  std::printf("  crickets at 10 m: noon %.4f, 23:00 %.4f (correlation %.3f); PreRaceAmbience 1 %.5f; at 70 m %.5f\n",
              e_noon, e_night, c_tod, e_pre, e_far);
  CHECK(e_noon > 1e-3 && e_night > 1e-3 && std::fabs(c_tod) < 0.9);
  CHECK(e_pre < 0.05 * e_noon);
  CHECK(e_far < 0.05 * e_noon);

  // Race state on a placed Default_Race_Crowd_Amb_A (InRace window 0.4..1).
  const auto crowd = emitter("Default_Race_Crowd_Amb_A", 5.0f);
  audio::AmbienceParams racing = noon;
  racing.in_race = 1.0f;
  const double c_free = energy(crowd, noon, nullptr), c_race = energy(crowd, racing, nullptr);
  std::printf("  race crowd at 5 m: free roam %.5f, in race %.4f\n", c_free, c_race);
  CHECK(c_race > 1e-3 && c_free < 0.05 * c_race);

  // Default_Crowd_Grandstand (620 placed): InRace gates it, PreRaceAmbience
  // ducks it to 0.063.
  const auto stand = emitter("Default_Crowd_Grandstand", 5.0f);
  audio::AmbienceParams racing_pre = racing;
  racing_pre.pre_race_ambience = 1.0f;
  const double g_race = energy(stand, racing, nullptr), g_pre = energy(stand, racing_pre, nullptr);
  std::printf("  grandstand at 5 m in race: %.4f, PreRaceAmbience 1 %.5f\n", g_race, g_pre);
  CHECK(g_race > 1e-3 && g_pre < 0.05 * g_race);

  // The path the game takes: the placements, the pool
  // WorldAcousticsData::evaluate draws from them on each event's own reach,
  // and the player rendering that sample.
  std::map<std::string, std::array<int, 3>> census;  // placements, resolved, playable
  int n_place = 0, n_resolved = 0, n_playable = 0;
  for (const auto& em : world.emitters()) {
    auto& c = census[em.event_name];
    const audio::AmbienceEventReach* r = world.event_reach(audio::ambience_event_hash(em.event_name));
    ++c[0];
    ++n_place;
    if (r) ++c[1], ++n_resolved;
    if (r && r->playable) ++c[2], ++n_playable;
  }
  std::printf("  placements: %d (%d tiles); %d name a disc event, %d of those have a loop\n", n_place, tiles,
              n_resolved, n_playable);
  std::vector<std::pair<int, std::string>> missing;
  for (const auto& [name, c] : census)
    if (c[1] == 0) missing.push_back({c[0], name});
  std::sort(missing.rbegin(), missing.rend());
  for (size_t k = 0; k < missing.size() && k < 12; ++k)
    std::printf("    no event on the disc: %s (%d)\n", missing[k].second.c_str(), missing[k].first);
  CHECK(census["Default_Crickets_Bed"][2] > 1000);

  // Ten metres from a placed Default_Crickets_Bed, it is in the pool.
  const uint32_t h_crickets = audio::ambience_event_hash("Default_Crickets_Bed");
  std::vector<const audio::AmbientEmitter3D*> beds;
  for (const auto& em : world.emitters())
    if (em.event_hash == h_crickets) beds.push_back(&em);
  int tried = 0, pooled = 0;
  for (size_t k = 0; k < beds.size(); k += std::max<size_t>(1, beds.size() / 200)) {
    const auto& em = *beds[k];
    const auto smp = world.evaluate(em.pos1[0] + 10.0f, em.pos1[1], em.pos1[2]);
    ++tried;
    for (const auto& a : smp.active_emitters)
      if (a.active && a.event_hash == h_crickets) {
        ++pooled;
        break;
      }
  }
  std::printf("  10 m from a Default_Crickets_Bed: in the pool at %d of %d placements\n", pooled, tried);
  CHECK(tried > 0 && pooled * 10 >= tried * 9);

  // That sample through the player: the crickets at 23:00 over the region
  // bed (40 mph, where the quad bed is out).
  const auto& c0 = *beds[0];
  const auto at_bed = audio::RtAcousticsSample::from_sample(world.evaluate(c0.pos1[0] + 10.0f, c0.pos1[1], c0.pos1[2]));
  const double w_night = energy(at_bed, night, nullptr);
  std::printf("  evaluated sample at a crickets bed, 23:00: %.4f (no emitter %.6f)\n", w_night,
              energy(quiet, night, nullptr));
  CHECK(w_night > 1e-3);

  // The festival: its ribbon placements (stage music, stage crowd) and the
  // tiles' festival events, at the stage, through the same path.
  const audio::AmbientEmitter3D* stage = nullptr;
  int fest = 0, fest_resolved = 0;
  for (const auto& em : world.emitters()) {
    if (em.event_name.rfind("Festival", 0) != 0) continue;
    ++fest;
    if (world.event_reach(em.event_hash)) ++fest_resolved;
    if (!stage && em.event_name == "Festival_Stage_Music_A") stage = &em;
  }
  std::printf("  festival placements: %d, %d name a disc event\n", fest, fest_resolved);
  CHECK(stage != nullptr);
  if (stage) {
    const auto smp = world.evaluate(stage->pos1[0] + 5.0f, stage->pos1[1], stage->pos1[2]);
    int live = 0;
    for (const auto& a : smp.active_emitters) live += a.active ? 1 : 0;
    const auto rt = audio::RtAcousticsSample::from_sample(smp);
    const double f_noon = energy(rt, noon, nullptr);
    std::printf("  5 m from the festival stage: %d pooled, snapshot %s, energy %.4f (no emitter %.6f)\n", live,
                smp.ambience_snapshot.c_str(), f_noon, energy(quiet, noon, nullptr));
    CHECK(live > 0 && f_noon > 1e-3);
  }

  // Walking along the crickets beds: the pool's order changes, a voice
  // follows its emitter and one dropped fades (no block steps by more than
  // a click's worth).
  double worst = 0.0;
  float prev_l = 0.0f;
  bool have_prev = false;
  for (int step = 0; step < 120; ++step) {
    const auto& em = *beds[size_t(step / 20) % beds.size()];
    const float t = float(step % 20) * 3.0f;
    const auto rt = audio::RtAcousticsSample::from_sample(world.evaluate(em.pos1[0] + t, em.pos1[1], em.pos1[2] + t));
    std::fill(buf.begin(), buf.end(), 0.0f);
    amb.render(rt, night, buf.data(), kBlock);
    if (step % 20 == 0) {
      have_prev = false;  // the walk jumps to the next bed
      continue;
    }
    if (have_prev) worst = std::max(worst, double(std::fabs(buf[0] - prev_l)));
    prev_l = buf[size_t(2 * (kBlock - 1))];
    have_prev = true;
  }
  std::printf("  walking the crickets beds: largest step between blocks %.4f\n", worst);
  CHECK(worst < 0.25);
}

// FE-01: the title's music. ui4audio.xml's SplashScreen plays
// UIInGame/UI/FrontEnd/FE_Ambience (looping), a music-category event of one
// sound, PressStart_Music.wav in UIInGame_Streams (sub-sound 22, 338.353 s).
// The engine's title bed (AudioEngine::UiMusicMode::kTitle) is the first
// sub-sound named like "pressstart_music": that one.
void corpus_title_music(const std::filesystem::path& game) {
  const auto dir = game / "media" / "audio" / "UI" / "UIInGame";
  audio::FevProject proj;
  std::string err;
  CHECK(audio::load_fev(dir / "UIInGame.fev", proj, &err));
  const audio::FevEvent* ev = proj.find_event("UIInGame/UI/FrontEnd/FE_Ambience");
  CHECK(ev != nullptr);
  if (!ev) return;
  CHECK(ev->categories.size() == 1 && ev->categories[0] == "music");
  CHECK(std::fabs(ev->volume - 1.0f) < 1e-4f);
  const audio::FevSoundInstance* inst = nullptr;
  int count = int(ev->simple_instances.size());
  if (!ev->simple_instances.empty()) inst = &ev->simple_instances[0];
  for (const auto& ly : ev->layers) {
    count += int(ly.instances.size());
    if (!inst && !ly.instances.empty()) inst = &ly.instances[0];
  }
  CHECK(count == 1 && inst != nullptr && inst->sound_def_index < proj.sound_defs.size());
  if (!inst || inst->sound_def_index >= proj.sound_defs.size()) return;
  const auto& sd = proj.sound_defs[inst->sound_def_index];
  CHECK(sd.entries.size() == 1);
  if (sd.entries.size() != 1) return;
  const auto& en = sd.entries[0];
  CHECK(en.wavebank_name == "UIInGame_Streams" && en.subsound_index == 22 && en.length_ms == 338353);
  CHECK(en.source_wav.find("PressStart_Music") != std::string::npos);

  std::ifstream bf(dir / "UIInGame_Streams.fsb", std::ios::binary);
  CHECK(bool(bf));
  if (!bf) return;
  bf.seekg(0, std::ios::end);
  const uint64_t size = uint64_t(bf.tellg());
  bf.seekg(0);
  std::vector<uint8_t> head(size_t(std::min<uint64_t>(size, 1u << 20)));
  bf.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  std::vector<audio::FsbSample> samples;
  CHECK(audio::read_fsb4(head, samples, &err, size));
  int first = -1;
  for (size_t i = 0; i < samples.size() && first < 0; ++i) {
    std::string nm = samples[i].name;
    for (char& c : nm) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (nm.find("pressstart_music") != std::string::npos) first = int(i);
  }
  std::printf("  title music: FE_Ambience -> %s, sub-sound %u; the engine's sample %d\n", sd.name.c_str(),
              en.subsound_index, first);
  CHECK(first == int(en.subsound_index));
}

int run_corpus() {
  const char* game = std::getenv("SHIFT_GAME_ROOT");
  if (!game || !*game || !std::filesystem::exists(game)) {
    std::printf("audio parity corpus: skipped (no SHIFT_GAME_ROOT)\n");
    return 77;
  }
  const std::filesystem::path root(game);
  corpus_title_music(root);
  corpus_damage_event(root);
  corpus_tyre_wind_events(root);
  corpus_ui_catalog(root);
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  corpus_other_car_banks(root, state && *state ? std::filesystem::path(state) : std::filesystem::path{});
  corpus_ambience_events(root);
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps what was printed
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
