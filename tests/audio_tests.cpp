// SPEC-ENGINE-SOUND: the synthetic rule checks (no game data), the reader
// and the player, then the R34's loops at 4,000 and 7,000 rpm over the
// player's files when SHIFT_GAME_ROOT names the disc.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/bake.h"
#include "audio/engine_player.h"
#include "audio/event_configs.h"
#include "audio/fev.h"
#include "audio/fsb4.h"
#include "audio/mixer.h"
#include "audio/rt.h"
#include "audio/snapshots.h"
#include "audio/sources.h"
#include "audio/user_music.h"
#include "audio/xml.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

audio::Loop loop(int lo, int sample, int hi) {
  audio::Loop l;
  l.rpm_min = lo, l.rpm_sample = sample, l.rpm_max = hi;
  return l;
}

void rule_checks() {
  // Two loops 1,000-2,200 and 2,000-3,000 rpm (spec Tests).
  const std::vector<audio::Loop> two = {loop(1000, 1500, 2200), loop(2000, 2500, 3000)};
  auto g = audio::crossfade(two, 2100);
  CHECK(near(g[0], 0.585, 0.001) && near(g[1], 0.585, 0.001));
  g = audio::crossfade(two, 1500);
  CHECK(g[0] == 1 && g[1] == 0);
  g = audio::crossfade(two, 2000);
  CHECK(g[0] == 1 && g[1] == 0);
  // Pitch: rpm_sample 3,000 plays 1.2 at 3,600 and 0.8 at 2,400; pitchMax
  // 0.5 gives 1.1 at 3,600.
  audio::Loop p = loop(0, 3000, 32000);
  CHECK(near(audio::pitch_ratio(p, 3600), 1.2, 1e-12) && near(audio::pitch_ratio(p, 2400), 0.8, 1e-12));
  p.pitch_max = 0.5;
  CHECK(near(audio::pitch_ratio(p, 3600), 1.1, 1e-12));
  // A parameter (RPM 0.5, Throttle 0.5) through (0, 0.2), (0.5, 0.5), (1, 1):
  // n 0.4 and throttle 1 give x 0.7 and y 0.7.
  audio::Param q;
  q.present = true;
  q.rpm = 0.5, q.throttle = 0.5;
  q.x = {0, 0.5, 1}, q.y = {0.2, 0.5, 1};
  CHECK(near(q.eval({0.4, 1, 0, 0}), 0.7, 1e-12));
  // The rpm map: the R34's 800-8,000 physics onto 800-7,800 audio.
  double n = 0;
  CHECK(near(audio::audio_rpm(4000, 800, 8000, 800, 7800, &n), 3911.1, 0.1) && near(n, 0.4444, 1e-4));
  // The loop index: past n samples, the second channel; past a mono bank,
  // the same sample.
  std::vector<audio::Loop> idx(3);
  idx[0].bank_index = 1, idx[1].bank_index = 13, idx[2].bank_index = 14;
  audio::resolve_loop_indices(idx, 12, std::vector<int>(12, 2));
  CHECK(idx[0].sample == 1 && idx[0].channel == 0 && idx[1].sample == 1 && idx[1].channel == 1);
  audio::resolve_loop_indices(idx, 12, std::vector<int>(12, 1));
  CHECK(idx[2].sample == 2 && idx[2].channel == 0);
  CHECK(audio::silent_name("NA.xml") && audio::silent_name("na_Exhaust_HT.xml") && audio::silent_name("NA.fsb") &&
        !audio::silent_name("6T2_RB26_Exh.fsb"));
}

void xml_checks() {
  audio::XmlNode root;
  std::string e;
  CHECK(audio::parse_xml("\r\n<?xml version=\"1.0\"?><!-- c --><A x=\"1.5\"><b y='a&amp;b'/><D><c/></D>text</A>", root,
                         &e));
  CHECK(root.name == "A" && near(root.number("X"), 1.5, 0) && root.child("B") && root.child("b")->attr("y") == "a&b");
  CHECK(root.find("d/C") && !root.find("b/c"));
  CHECK(!audio::parse_xml("<A><b></A>", root, &e));
}

void fsb_checks() {
  // A one-sample bank: 48-byte header, an 80-byte sample header, data at 128.
  std::vector<uint8_t> bank(128 + 4096, 0);
  auto put32 = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) bank[at + size_t(i)] = uint8_t(v >> (8 * i));
  };
  std::copy_n("FSB4", 4, bank.begin());
  put32(4, 1), put32(8, 80), put32(12, 4096);
  bank[48] = 80;
  std::copy_n("EngB_1000", 9, bank.begin() + 50);
  put32(48 + 32, 8192), put32(48 + 36, 4096), put32(48 + 48, 0x01000000), put32(48 + 52, 36000);
  bank[48 + 62] = 1;
  std::vector<audio::FsbSample> s;
  std::string e;
  CHECK(audio::read_fsb4(bank, s, &e) && s.size() == 1);
  if (s.size() == 1) {
    CHECK(s[0].name == "EngB_1000" && s[0].frames == 8192 && s[0].rate == 36000 && s[0].xma() && s[0].offset == 128);
    const auto w = audio::xma2_wave(s[0], std::span<const uint8_t>(bank).subspan(128, 4096));
    CHECK(w.size() == 12 + 8 + 52 + 8 + 4096 && w[20] == 0x66 && w[21] == 0x01);
  }
}

void player_checks() {
  // One group, two loops; a 100 Hz tone at 1,000 rpm in each.
  audio::EngineSound sound;
  sound.audio_rpm_idle = 1000, sound.audio_rpm_redline = 3000;
  audio::Group g;
  g.name = "EngineAmbient";
  g.loops = {loop(500, 1000, 2200), loop(2000, 2500, 32000)};
  sound.groups.push_back(g);
  audio::PcmSample tone;
  tone.rate = 48000;
  for (int i = 0; i < 480; ++i) tone.data.push_back(float(std::sin(2 * 3.14159265358979 * i / 480)));
  audio::EnginePlayer player(sound, {{&tone, &tone}}, 48000, 1000, 3000);
  player.set({1000, 0, 0, 0});
  std::vector<float> out(2 * 4800);
  player.render(out.data(), 4800);
  double energy = 0;
  int crossings = 0;
  for (int f = 2400; f < 4800; ++f) {
    energy += out[2 * size_t(f)] * out[2 * size_t(f)];
    crossings += (out[2 * size_t(f) - 2] < 0) != (out[2 * size_t(f)] < 0);
  }
  CHECK(energy > 100 && player.voices() == 1);
  CHECK(crossings >= 9 && crossings <= 11);  // 100 Hz over 50 ms: 10 zero crossings
  // Up to 2,500 rpm: the first loop leaves its range and fades out in about
  // 0.15 s (x 0.2 per 1/30 s).
  player.set({2500, 0, 0, 0});
  player.render(out.data(), 4800);  // 0.1 s
  CHECK(player.voices() == 2);
  player.render(out.data(), 4800);
  CHECK(player.voices() == 1);

  // Setting master = 0.0 ramps output smoothly to silence (< 1e-3 RMS after 40 ms
  // with the 5 ms smoother), without NaNs, and recovers when master is restored.
  player.master = 0.0;
  player.set({2500, 0, 0, 0});
  std::vector<float> mute_buf(2 * 2400);
  player.render(mute_buf.data(), 2400);  // 50 ms
  double tail_sq = 0.0;
  for (int f = 1920; f < 2400; ++f) {
    const float l = mute_buf[2 * size_t(f)];
    const float r = mute_buf[2 * size_t(f) + 1];
    CHECK(std::isfinite(l) && std::isfinite(r));
    tail_sq += double(l) * l + double(r) * r;
  }
  const double tail_rms = std::sqrt(tail_sq / (2 * 480));
  CHECK(tail_rms < 1e-3);

  player.master = 0.6;
  player.set({2500, 0, 0, 0});
  player.render(mute_buf.data(), 2400);
  double rec_sq = 0.0;
  for (int f = 1920; f < 2400; ++f) {
    const float l = mute_buf[2 * size_t(f)];
    rec_sq += double(l) * l;
  }
  CHECK(std::sqrt(rec_sq / 480) > 1e-2);
}

void r34_checks() {
  const char* game = std::getenv("SHIFT_GAME_ROOT");
  if (!game) {
    std::printf("r34: skipped (no SHIFT_GAME_ROOT)\n");
    return;
  }
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  cars::Install install;
  audio::SoundFiles files;
  audio::LoadedSound loaded;
  std::string e;
  CHECK(cars::discover_install(game, state ? state : "", {}, install, &e) && files.open(install, &e) &&
        audio::load_engine_sound(files, "NIS_SkylineGTRVSpecII_02", loaded, &e));
  if (!e.empty()) std::printf("r34: %s\n", e.c_str());
  const auto& s = loaded.sound;
  std::printf("r34: audio rpm %.0f-%.0f, %zu groups\n", s.audio_rpm_idle, s.audio_rpm_redline, s.groups.size());
  CHECK(s.groups.size() == 4);
  // The spec's table at 4,000 physics rpm (audio 3,911) and 7,000 (6,828),
  // the physics range StallRPM 800 to RedlineRPM 8,000.
  struct Row {
    const char* group;
    double rpm;
    int a, b;  // loops sounding (b -1: a alone)
    double ga, gb, pa, pb;
  };
  const Row rows[] = {
      {"EngineIntake", 4000, 5, 6, 0.596, 0.574, 1.111, 0.908},
      {"EngineAmbient", 4000, 5, -1, 1, 0, 1.025, 0},
      {"ExhaustL", 4000, 5, -1, 1, 0, 1.025, 0},
      {"ExhaustR", 4000, 5, -1, 1, 0, 1.025, 0},
      {"EngineAmbient", 7000, 10, 11, 0.523, 0.644, -1, -1},
      {"ExhaustL", 7000, 10, 11, 0.523, 0.644, -1, -1},
      {"EngineIntake", 7000, 11, -1, 1, 0, -1, -1},
  };
  for (const Row& row : rows) {
    const audio::Group* g = nullptr;
    for (const auto& x : s.groups)
      if (x.name == row.group) g = &x;
    CHECK(g != nullptr);
    if (!g) continue;
    const double r = audio::audio_rpm(row.rpm, 800, 8000, s.audio_rpm_idle, s.audio_rpm_redline, nullptr);
    const auto gains = audio::crossfade(g->loops, r);
    std::string sounding;
    for (size_t i = 0; i < gains.size(); ++i)
      if (gains[i] > 0) sounding += " " + std::to_string(i) + ":" + std::to_string(gains[i]).substr(0, 5);
    std::printf("r34 %s at %.0f rpm (audio %.0f):%s\n", row.group, row.rpm, r, sounding.c_str());
    for (size_t i = 0; i < gains.size(); ++i) {
      const bool expected = int(i) == row.a || int(i) == row.b;
      CHECK(expected == (gains[i] > 0));
    }
    CHECK(near(gains[size_t(row.a)], row.ga, 0.0015));
    if (row.b >= 0) CHECK(near(gains[size_t(row.b)], row.gb, 0.0015));
    if (row.pa > 0) CHECK(near(audio::pitch_ratio(g->loops[size_t(row.a)], r), row.pa, 0.001 * row.pa));
    if (row.pb > 0) CHECK(near(audio::pitch_ratio(g->loops[size_t(row.b)], r), row.pb, 0.001 * row.pb));
  }
  // The exhaust's right loops play the stereo samples' second channel.
  for (const auto& g : s.groups)
    if (g.name == "ExhaustR") CHECK(!g.loops.empty() && g.loops[0].channel == 1 && g.loops[0].sample == 0);

  // Upgrade levels L1..L3 resolve cleanly on the R34 (and runtime XMA2 decode
  // works without any prior soundbake when ffmpeg is present).
  std::string err;
  audio::LoadedSound upgraded;
  CHECK(audio::load_engine_sound(files, "NIS_SkylineGTRVSpecII_02", upgraded, &err, {3, 3}));
  CHECK(!upgraded.sound.groups.empty());
  if (!audio::find_ffmpeg().empty()) {
    namespace fs = std::filesystem;
    const fs::path cache = fs::temp_directory_path() / "shift-rt-xma2-test";
    std::error_code ec;
    fs::remove_all(cache, ec);
    audio::BakedSound decoded;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok =
        audio::decode_engine_sound_runtime(files, "NIS_SkylineGTRVSpecII_02", cache, decoded, &err, {2, 2});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("r34 runtime XMA2 decode (L2/L2): %s, %zu pcm loops in %.0f ms\n", ok ? "ok" : err.c_str(),
                decoded.pcm.size(), ms);
    CHECK(ok && !decoded.pcm.empty() && !decoded.loaded.sound.groups.empty());
    // Second load hits the cache without running ffmpeg again.
    audio::BakedSound cached;
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(audio::decode_engine_sound_runtime(files, "NIS_SkylineGTRVSpecII_02", cache, cached, &err, {2, 2}));
    const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("r34 cached reload (L2/L2): %.1f ms\n", ms2);
    CHECK(cached.pcm.size() == decoded.pcm.size());
    fs::remove_all(cache, ec);
  }
}

void upgrade_level_checks() {
  const char* et = R"(
    <Engine>
      <EngineSettings audio_rpm_idle="800" audio_rpm_redline="7000" />
      <EngineIntake><Upgrade L0="in0.xml" L1="in1.xml" L2="in2.xml" L3="in3.xml" /></EngineIntake>
      <EngineAmbient><Upgrade L0="amb0.xml" /></EngineAmbient>
      <Exhaust><Upgrade L0="ex0.xml" L1="ex1.xml" L2="ex2.xml" L3="ex3.xml" /></Exhaust>
    </Engine>
  )";
  audio::XmlNode root;
  std::string err;
  CHECK(audio::parse_xml(et, root, &err));
  audio::EngineSound s;
  std::vector<audio::HarmonicRef> refs;
  CHECK(audio::read_engine_tuning(root, s, refs, &err, {0, 0}));
  CHECK(refs.size() == 3 && refs[0].file == "in0.xml" && refs[1].file == "amb0.xml" && refs[2].file == "ex0.xml");
  CHECK(audio::read_engine_tuning(root, s, refs, &err, {2, 3}));
  CHECK(refs.size() == 3 && refs[0].file == "in2.xml" && refs[1].file == "amb0.xml" && refs[2].file == "ex3.xml");
  // Out-of-range levels clamp to 0..3.
  CHECK(audio::read_engine_tuning(root, s, refs, &err, {9, -1}));
  CHECK(refs.size() == 3 && refs[0].file == "in3.xml" && refs[2].file == "ex0.xml");
  CHECK(audio::baked_sound_key("CarA", {0, 0}) == "CarA");
  CHECK(audio::baked_sound_key("CarA", {1, 3}) == "CarA_I1_E3");
}

// The real-time hand-off primitives: order, capacity, latest-wins, and a
// producer/consumer pair across two threads.
void rt_checks() {
  audio::SpscQueue<int> q(4);
  CHECK(q.capacity() == 4);
  for (int i = 0; i < 4; ++i) CHECK(q.push(i));
  CHECK(!q.push(9));  // full: nothing lost silently
  int v = -1;
  for (int i = 0; i < 4; ++i) CHECK(q.pop(v) && v == i);
  CHECK(!q.pop(v));

  audio::TripleBuffer<int> t(7);
  CHECK(!t.read(v) && v == 7);
  t.write(1);
  t.write(2);
  t.write(3);
  CHECK(t.read(v) && v == 3);  // the latest only
  CHECK(!t.read(v) && v == 3);

  // Across threads: every value arrives in order; the latest-wins buffer
  // never shows a torn or older value.
  audio::SpscQueue<uint64_t> big(1024);
  std::atomic<bool> bad{false};
  std::thread consumer([&] {
    uint64_t expect = 0, got = 0;
    while (expect < 20000) {
      if (big.pop(got)) {
        if (got != expect) bad = true;
        ++expect;
      }
    }
  });
  for (uint64_t i = 0; i < 20000;) i += big.push(i) ? 1 : 0;
  consumer.join();
  CHECK(!bad);

  struct Pair {
    uint64_t a, b;
  };
  audio::TripleBuffer<Pair> tb;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (uint64_t i = 1; !stop; ++i) tb.write({i, i * 3});
  });
  uint64_t last = 0;
  bool torn = false, backwards = false;
  for (int i = 0; i < 200000; ++i) {
    Pair p{};
    tb.read(p);
    if (p.b != p.a * 3) torn = true;
    if (p.a < last) backwards = true;
    last = p.a;
  }
  stop = true;
  writer.join();
  CHECK(!torn && !backwards);
}

// Buses, the snapshots' product and fades, master volume and mute, and the
// compressor against Full Dynamic Range.
void mixer_checks() {
  using audio::Bus;
  Bus b{};
  CHECK(audio::bus_from_name("vo", b) && b == Bus::kVo);
  CHECK(audio::bus_from_name("OtherCars", b) && b == Bus::kOtherCars);
  CHECK(!audio::bus_from_name("Nope", b));

  const int rate = 48000, n = rate / 2;  // half a second blocks
  std::vector<float> engine(2 * size_t(n), 0.1f), music(2 * size_t(n), 0.2f), out(2 * size_t(n));
  const float* buses[audio::kBuses] = {};
  buses[int(Bus::kEngine)] = engine.data();
  buses[int(Bus::kMusic)] = music.data();

  audio::Mixer m;
  m.prepare(rate);
  m.snapshots().add_defaults();
  m.stage().set_full_dynamic_range(true);
  m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.3, 1e-3));  // the buses add

  // A bus's Sound Mix level scales it (0..2), alone.
  m.set_bus_level(Bus::kMusic, 2.0f);
  m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.5, 1e-3));
  m.set_bus_level(Bus::kMusic, 0.0f);
  m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.1, 1e-3));
  m.set_bus_level(Bus::kMusic, 1.0f);

  // Pause: the world drops out over its fade, the music dips to 0.25.
  const int paused = m.snapshots().find("Paused");
  CHECK(paused >= 0);
  m.snapshots().set_active(paused, true);
  m.mix(buses, n, out.data());  // 0.5 s covers the 0.15 s fade
  m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.05, 2e-3));
  m.snapshots().set_active(paused, false);
  m.mix(buses, n, out.data());
  m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.3, 2e-3));

  // Snapshots multiply: Paused (music 0.25) and Menu (music 0) -> 0.
  m.snapshots().set_active(paused, true);
  m.snapshots().set_active(m.snapshots().find("Menu"), true);
  for (int i = 0; i < 3; ++i) m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0, 1e-3));
  m.snapshots().set_active(paused, false);
  m.snapshots().set_active(m.snapshots().find("Menu"), false);
  for (int i = 0; i < 3; ++i) m.mix(buses, n, out.data());

  // Master volume and mute (smoothed: silent by the block's end).
  m.set_master(0.5f);
  m.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.15, 1e-3));
  m.set_mute(true);
  m.mix(buses, n, out.data());
  CHECK(std::fabs(out[2 * n - 1]) < 1e-4);
  m.set_mute(false);
  m.set_master(1.0f);
  for (int i = 0; i < 2; ++i) m.mix(buses, n, out.data());

  // Compressor: a loud steady signal is reduced; Full Dynamic Range leaves
  // it; a quiet one is untouched either way.
  std::vector<float> loud(2 * size_t(n), 0.8f), quiet(2 * size_t(n), 0.05f);
  buses[int(Bus::kEngine)] = loud.data();
  buses[int(Bus::kMusic)] = nullptr;
  audio::Mixer c;
  c.prepare(rate);
  c.mix(buses, n, out.data());
  c.mix(buses, n, out.data());
  const float compressed = out[2 * n - 1];
  CHECK(compressed < 0.7f && compressed > 0.3f);
  CHECK(c.stage().gain_reduction_db() > 1.0f);
  c.stage().set_full_dynamic_range(true);
  for (int i = 0; i < 3; ++i) c.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.8, 1e-3));
  CHECK(c.stage().gain_reduction_db() < 0.05f);
  c.stage().set_full_dynamic_range(false);
  buses[int(Bus::kEngine)] = quiet.data();
  for (int i = 0; i < 3; ++i) c.mix(buses, n, out.data());
  CHECK(near(out[2 * n - 1], 0.05, 1e-3));

  // The limiter holds full-scale sums under 1 without hard clipping.
  CHECK(near(audio::soft_limit(0.5f), 0.5, 1e-6));
  CHECK(audio::soft_limit(3.0f) <= 1.0f && audio::soft_limit(3.0f) > 0.99f);
  CHECK(audio::soft_limit(-3.0f) < -0.99f);

  // Confirmed NegTorque sign: braking raises x whether passed as -0.5 or +0.5.
  audio::Param neg_p;
  neg_p.present = true;
  neg_p.neg_torque = 1.0;
  neg_p.x = {0.0, 0.5, 1.0};
  neg_p.y = {0.2, 0.6, 1.0};
  CHECK(near(neg_p.eval({0.0, 0.0, 0.0, -0.5}), 0.6, 1e-6));
  CHECK(near(neg_p.eval({0.0, 0.0, 0.0, 0.5}), 0.6, 1e-6));

  // Engine DSP: LoadPEQ throttle interpolation, PEQ idle_gain_reset, Expander
  // timing (Attack 20ms, Hold 100ms, Release 80ms, MaxGain 0.5), IntakeChuff.
  audio::EngineSound ds;
  ds.audio_rpm_idle = 800;
  ds.audio_rpm_redline = 7000;
  audio::Group eg;
  eg.name = "ExhaustL";
  eg.idle_gain_reset = 1001;  // blend from 800 to 1000
  eg.loops.push_back(loop(500, 1500, 8000));
  eg.peq.active = true;
  eg.peq.gain.present = true;
  eg.peq.gain.y = {2.0, 2.0, 2.0};
  eg.peq.center_freq.present = true;
  eg.peq.center_freq.y = {400, 400, 400};
  eg.peq.bandwidth.present = true;
  eg.peq.bandwidth.y = {1.0, 1.0, 1.0};
  eg.load_peq.active = true;
  eg.load_peq.pos_load.active = true;
  eg.load_peq.pos_load.gain.present = true;
  eg.load_peq.pos_load.gain.y = {1.8, 1.8, 1.8};
  eg.load_peq.pos_load.center_freq.present = true;
  eg.load_peq.pos_load.center_freq.y = {350, 350, 350};
  eg.load_peq.pos_load.bandwidth.present = true;
  eg.load_peq.pos_load.bandwidth.y = {2.0, 2.0, 2.0};
  eg.load_peq.neg_load.active = true;
  eg.load_peq.neg_load.gain.present = true;
  eg.load_peq.neg_load.gain.y = {0.8, 0.8, 0.8};
  eg.load_peq.neg_load.center_freq.present = true;
  eg.load_peq.neg_load.center_freq.y = {450, 450, 450};
  eg.load_peq.neg_load.bandwidth.present = true;
  eg.load_peq.neg_load.bandwidth.y = {1.0, 1.0, 1.0};
  eg.expander.present = true;
  eg.expander.max_gain.present = true;
  eg.expander.max_gain.y = {0.5, 0.5, 0.5};
  eg.expander.attack_ms = 20.0;
  eg.expander.hold_ms = 100.0;
  eg.expander.release_ms = 80.0;
  ds.groups.push_back(eg);

  audio::PcmSample chuff;
  chuff.rate = 48000;
  chuff.data.assign(2400, 0.25f);
  audio::PcmSample loop_pcm;
  loop_pcm.rate = 48000;
  loop_pcm.data.assign(4800, 0.2f);
  audio::EnginePlayer dp(ds, {{&loop_pcm}}, 48000, 800, 7000, &chuff);

  // Idle gain reset at 800 -> 1.0, at 900 (halfway to 1000) -> 1.5, at 2000 -> 2.0.
  dp.set({800, 0.0, 0.0, 0.0, audio::View::kFollow});
  CHECK(near(dp.peq_gain(0), 1.0, 1e-4));
  CHECK(!dp.intake_chuff_active());
  // Snap open throttle from 0.0 -> 0.5 at low rpm (n < 0.425): IntakeChuff triggers!
  dp.set({900, 0.5, 0.5, 0.0, audio::View::kFollow});
  CHECK(near(dp.peq_gain(0), 1.5, 1e-3));
  CHECK(near(dp.load_peq_gain(0), 1.3, 1e-4));  // lerp(0.8, 1.8, 0.5) = 1.3
  CHECK(dp.intake_chuff_active());

  // Expander timing: t=10ms -> 0, t=50ms -> 0.5, t=150ms -> 0.5, t=200ms -> 0.
  dp.trigger_expander();
  dp.advance_dsp(0.010);
  CHECK(near(dp.expander_state(0), 0.0, 1e-5));
  dp.advance_dsp(0.040);  // total 50 ms
  CHECK(near(dp.expander_state(0), 0.5, 1e-5));
  dp.advance_dsp(0.100);  // total 150 ms
  CHECK(near(dp.expander_state(0), 0.5, 1e-5));
  dp.advance_dsp(0.050);  // total 200 ms
  CHECK(near(dp.expander_state(0), 0.0, 1e-5));

  // Disc check: VW_IECorrado_95 falls back to VW_BORAVR6_03; R34 has IntakeChuff & Expander/LoadPEQ.
  if (const char* game = std::getenv("SHIFT_GAME_ROOT")) {
    cars::Install inst;
    audio::SoundFiles sf;
    std::string e;
    if (cars::discover_install(game, {}, {}, inst, &e) && sf.open(inst, &e)) {
      audio::LoadedSound corrado;
      CHECK(audio::load_engine_sound(sf, "VW_IECorrado_95", corrado, &e));
      CHECK(!corrado.sound.groups.empty());
      audio::LoadedSound r34;
      CHECK(audio::load_engine_sound(sf, "NIS_SkylineGTRVSpecII_02", r34, &e));
      CHECK(r34.sound.intake_chuff_sample >= 0);
      bool saw_peq = false, saw_load_peq = false, saw_exp = false;
      for (const auto& g : r34.sound.groups) {
        saw_peq |= g.peq.active;
        saw_load_peq |= g.load_peq.active;
        saw_exp |= g.expander.present;
      }
      CHECK(saw_peq && saw_load_peq && saw_exp);
      CHECK(!r34.sound.burble.wavebank.empty());
    }
  }
}

void write_tone_wav(const std::filesystem::path& path, int rate, int frames, double freq_hz, double amp = 0.5) {
  std::vector<int16_t> pcm(size_t(frames) * 2);
  for (int i = 0; i < frames; ++i) {
    const double s = std::sin(2.0 * 3.14159265358979 * freq_hz * i / rate) * amp;
    const int16_t v = int16_t(std::lround(s * 32767.0));
    pcm[size_t(i) * 2] = v;
    pcm[size_t(i) * 2 + 1] = v;
  }
  const uint32_t data_bytes = uint32_t(pcm.size() * 2);
  const uint32_t riff_bytes = 36 + data_bytes;
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                   16,  0,   0,   0,   1, 0, 2, 0, 0,   0,   0,   0,   0,   0,   0,   0,
                   4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0};
  auto u32 = [](uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
  };
  u32(h + 4, riff_bytes);
  u32(h + 24, uint32_t(rate));
  u32(h + 28, uint32_t(rate * 4));
  u32(h + 40, data_bytes);
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(h), sizeof h);
  f.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(data_bytes));
}

// Your Music folder player: recursive scan, name order, shuffle, next/prev,
// and streaming WAV output.
void user_music_checks() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "shift-user-music-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "sub", ec);
  write_tone_wav(dir / "01_first.wav", 48000, 12000, 220.0, 0.4);
  write_tone_wav(dir / "sub" / "02_second.wav", 48000, 12000, 880.0, 0.4);
  {
    std::ofstream f(dir / "readme.txt");
    f << "ignore me";
  }

  audio::UserMusicPlayer um(dir, 48000, 123, false);
  CHECK(um.track_count() == 2 && !um.active());
  CHECK(um.current_title() == "01_first");
  um.set_enabled(true);
  CHECK(um.active());

  std::vector<float> buf(2 * 6000, 0.0f);
  um.render(buf.data(), 6000);
  double sq = 0;
  for (float x : buf) sq += double(x) * x;
  CHECK(std::sqrt(sq / double(buf.size())) > 0.1);

  um.next();
  CHECK(um.current_title() == "02_second");
  um.prev();
  CHECK(um.current_title() == "01_first");

  // Advancing past the end of track 1 moves to track 2 automatically.
  std::fill(buf.begin(), buf.end(), 0.0f);
  um.render(buf.data(), 6000);
  um.render(buf.data(), 6000);
  um.render(buf.data(), 6000);
  CHECK(um.current_title() == "02_second");

  um.set_shuffle(true);
  CHECK(um.shuffle() && um.current_title() == "02_second");
  um.set_enabled(false);
  CHECK(!um.active());
  fs::remove_all(dir, ec);
}

// Dedicated audio render thread: real-time sink, lock-free updates from the
// main thread, Your Music muting radio, and a no-stall budget check.
void audio_thread_checks() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "shift-audio-thread-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  write_tone_wav(dir / "track.wav", 48000, 48000, 440.0, 0.35);

  audio::EngineSound s;
  audio::Group g;
  g.name = "EngineAmbient";
  g.loops.push_back(loop(800, 2000, 8000));
  s.groups.push_back(g);
  audio::PcmSample tone;
  tone.rate = 48000;
  tone.data.resize(4800);
  for (size_t i = 0; i < tone.data.size(); ++i)
    tone.data[i] = float(0.4 * std::sin(2.0 * 3.14159265358979 * 100.0 * double(i) / 48000.0));

  audio::AudioEngine engine(48000);
  auto ep = std::make_unique<audio::EnginePlayer>(s, std::vector<std::vector<const audio::PcmSample*>>{{&tone}}, 48000,
                                                  800, 7000);
  ep->master = 0.6;
  engine.set_engine_player(std::move(ep));
  auto um = std::make_unique<audio::UserMusicPlayer>(dir, 48000, 7, true);
  um->set_enabled(true);
  engine.set_user_music(std::move(um));

  // Simulated 48 kHz playback sink draining at real-time pace.
  const auto start_tp = std::chrono::steady_clock::now();
  std::atomic<int64_t> written_frames{0};
  std::atomic<double> energy{0.0};
  std::atomic<bool> all_finite{true};

  audio::AudioEngine::Sink sink;
  sink.target_buffer_frames = 2400;  // 50 ms
  sink.block_frames = 480;           // 10 ms
  sink.queued_frames = [&]() -> int {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp).count();
    const int64_t consumed = int64_t(elapsed * 48000.0);
    return int(std::max<int64_t>(0, written_frames.load(std::memory_order_relaxed) - consumed));
  };
  sink.write = [&](const float* stereo, int frames) {
    double sq = 0;
    bool fin = true;
    for (int i = 0; i < frames * 2; ++i) {
      fin &= std::isfinite(stereo[i]);
      sq += double(stereo[i]) * stereo[i];
    }
    if (!fin) all_finite.store(false, std::memory_order_relaxed);
    energy.store(energy.load(std::memory_order_relaxed) + sq, std::memory_order_relaxed);
    written_frames.fetch_add(frames, std::memory_order_relaxed);
  };

  engine.start(std::move(sink));

  // Pump per-frame engine inputs and mixer commands from the main thread for
  // ~220 ms and verify neither thread stalls.
  double worst_main_ms = 0;
  for (int i = 0; i < 44; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    audio::EnginePlayer::Update u;
    u.rpm = 1200.0 + double(i) * 100.0;
    u.throttle = 0.5;
    u.pos_torque = 0.4;
    u.view = (i % 2 == 0) ? audio::View::kFollow : audio::View::kCockpit;
    engine.set_engine_input(u);
    engine.set_bus_level(audio::Bus::kEngine, 1.0f);
    if (i == 20) engine.set_full_dynamic_range(true);
    if (i == 30) engine.set_full_dynamic_range(false);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    worst_main_ms = std::max(worst_main_ms, ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const audio::AudioTelemetry tel = engine.telemetry();
  engine.stop();
  std::printf("audio thread: %llu blocks, worst block %.3f ms, stalls %llu, worst main push %.3f ms\n",
              (unsigned long long)tel.blocks_rendered, tel.worst_block_ms, (unsigned long long)tel.stalls,
              worst_main_ms);
  CHECK(all_finite.load());
  CHECK(energy.load() > 1.0);
  CHECK(tel.blocks_rendered >= 15);
  CHECK(tel.stalls == 0 && tel.worst_block_ms < 8.0);
  CHECK(worst_main_ms < 2.0);
  CHECK(tel.user_music_active && std::string(tel.user_music_title.data()) == "track");
  fs::remove_all(dir, ec);
}

// SPEC-AUDIO-EVENTS Part 3: 9x22 SnapshotMatrix, overrides, DynamicsData
// CrashData latch, GlobalDspConfig/CarBusEq slewing, and MasterRmsStage +
// soft saturator (with LFE bypass).
void snapshot_and_master_dsp_checks() {
  const char* xml = R"(
    <MixSnapshots>
      <DynamicsData>
        <MusicHighpass fadeinrate="3.0" fadeoutrate="33.0" zonefilterlevel="200.0"/>
        <CrashData threshold="50" drop="10"/>
        <TopSpeedData threshold="0.70"/>
      </DynamicsData>
      <FreeRoam group="Gameplay">
        <FocusCar active="1" destvolume="0.8" fadetime="0" returntime="0" returnfadetime="0"/>
        <GameUI active="1" destvolume="0.9" fadetime="0" returntime="0" returnfadetime="0"/>
      </FreeRoam>
      <Paused group="Gameplay">
        <FocusCar active="1" destvolume="0.0" fadetime="0" returntime="0" returnfadetime="0"/>
        <GameUI active="0" destvolume="0.1" fadetime="0" returntime="0" returnfadetime="0"/>
      </Paused>
      <RadioNormal group="Radio">
        <FocusCar active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
      </RadioNormal>
      <RadioDJSpeaking group="Radio">
        <FocusCar active="1" destvolume="0.4" fadetime="0" returntime="0" returnfadetime="0"/>
      </RadioDJSpeaking>
      <VONotPlaying group="VO">
        <FocusCar active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
      </VONotPlaying>
      <VOPlaying group="VO">
        <FocusCar active="1" destvolume="0.5" fadetime="0" returntime="0" returnfadetime="0"/>
      </VOPlaying>
      <VOPlayingOnUIExterior group="VO">
        <FocusCar active="1" destvolume="0.25" fadetime="0" returntime="0" returnfadetime="0"/>
      </VOPlayingOnUIExterior>
      <SatNavNotPlaying group="SatNav">
        <GameMusic active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
      </SatNavNotPlaying>
      <SatNavPlaying group="SatNav">
        <GameMusic active="1" destvolume="0.3" fadetime="0" returntime="0" returnfadetime="0"/>
      </SatNavPlaying>
      <Crash group="Impact">
        <FocusCar active="1" destvolume="0.2" fadetime="0.1" returntime="0.5" returnfadetime="0.4"/>
      </Crash>
      <LowSpeed group="Speed">
        <GameMusic active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
      </LowSpeed>
      <TopSpeed group="Speed">
        <GameMusic active="1" destvolume="0.9" fadetime="0" returntime="0" returnfadetime="0"/>
      </TopSpeed>
    </MixSnapshots>
  )";
  audio::SnapshotMatrix m;
  std::string err;
  CHECK(m.parse(xml, &err));
  CHECK(m.snapshot_count() == 12);

  // Snapshot multiplication: 0.8 * 0.4 * 0.5 = 0.16.
  CHECK(m.activate("RadioDJSpeaking"));
  CHECK(m.activate("VOPlaying"));
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.16, 1e-5));

  // SatNavPlaying ignored while RadioDJSpeaking active.
  CHECK(!m.activate("SatNavPlaying"));
  CHECK(near(m.gain(audio::Category::kGameMusic), 1.0, 1e-5));

  // active = 0 leaves GameUI unchanged (stays 0.9); VOPlaying while Paused -> VOPlayingOnUIExterior.
  CHECK(m.activate("Paused"));
  CHECK(near(m.gain(audio::Category::kGameUI), 0.9, 1e-5));
  CHECK(m.activate("VOPlaying"));
  CHECK(m.active_name(audio::SnapshotGroup::kVO) == "VOPlayingOnUIExterior");
  m.restore_gameplay();
  CHECK(m.active_name(audio::SnapshotGroup::kGameplay) == "FreeRoam");
  CHECK(m.activate("RadioNormal"));
  CHECK(m.activate("VONotPlaying"));
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8, 1e-5));

  // Return timing: fade 0.1 s -> hold 0.5 s -> return fade 0.4 s.
  CHECK(m.activate("Crash"));
  m.advance(0.1);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8 * 0.2, 1e-4));
  m.advance(0.5);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8 * 0.2, 1e-4));
  m.advance(0.2);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8 * 0.6, 1e-3));
  m.advance(0.2);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8, 1e-4));

  // CrashData latch: 60 -> 45 fires; 45 -> 30 does not; 0 -> 6 clears latch.
  m.reset_dynamics();
  m.update_dynamics(0.033, 60.0f, 140.0f);
  m.update_dynamics(0.033, 45.0f, 140.0f);
  m.advance(0.1);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8 * 0.2, 1e-4));
  m.advance(1.0);  // return complete
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8, 1e-4));
  m.update_dynamics(0.033, 30.0f, 140.0f);  // latched, no re-fire
  m.advance(0.1);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8, 1e-4));
  m.update_dynamics(0.033, 0.0f, 140.0f);   // saw slow
  m.update_dynamics(0.033, 6.0f, 140.0f);   // latch cleared
  m.update_dynamics(0.033, 65.0f, 140.0f);
  m.update_dynamics(0.033, 50.0f, 140.0f);  // re-fires!
  m.advance(0.1);
  CHECK(near(m.gain(audio::Category::kFocusCar), 0.8 * 0.2, 1e-4));

  // Master saturator exact formula: |x|*level=0.5, sat=0.9 (t=0.1) -> 0.5/1.4 = 0.35714286.
  CHECK(near(audio::master_saturate(0.5f, 1.0f, 0.9f, 1.0f), 0.5 / 1.4, 1e-5));
  CHECK(near(audio::master_saturate(-0.5f, 1.0f, 0.9f, 1.0f), -0.5 / 1.4, 1e-5));
  CHECK(near(audio::master_saturate(0.05f, 1.0f, 0.9f, 1.0f), 0.05, 1e-6));

  // Master RMS compressor with 5.1 LFE pass-through.
  audio::MasterRmsStage stage;
  stage.prepare(48000);
  audio::MasterDspConfig mdsp;
  mdsp.compressor_threshold = 0.1f;
  mdsp.compressor_ratio = 0.25f;
  mdsp.compressor_attack = 0.01f;
  mdsp.compressor_release = 0.1f;
  mdsp.saturator_bypass = true;
  stage.set_config(mdsp);
  std::vector<float> buf6(256 * 6, 0.0f);
  for (int blk = 0; blk < 30; ++blk) {
    for (int i = 0; i < 256; ++i) {
      buf6[size_t(i) * 6 + 0] = 0.5f;  // FL
      buf6[size_t(i) * 6 + 1] = 0.5f;  // FR
      buf6[size_t(i) * 6 + 2] = 0.5f;  // C
      buf6[size_t(i) * 6 + 3] = 0.5f;  // LFE (must pass untouched!)
      buf6[size_t(i) * 6 + 4] = 0.5f;  // RL
      buf6[size_t(i) * 6 + 5] = 0.5f;  // RR
    }
    stage.process(buf6.data(), 256, 6);
  }
  const float expected_g = (0.1f + (0.5f - 0.1f) * 0.25f) / 0.5f;  // 0.4
  CHECK(near(stage.current_gain(), expected_g, 0.02));
  CHECK(near(buf6[255 * 6 + 0], 0.5f * expected_g, 0.02));
  CHECK(near(buf6[255 * 6 + 3], 0.5f, 1e-5));  // LFE untouched!

  // CarBusEq slewing check.
  audio::CarBusEq eq;
  eq.prepare(48000);
  eq.set_target({false, 0.30f, 3.36f, 3772.0f});
  std::vector<float> st(4800 * 2, 0.1f);
  eq.process(st.data(), 4800);  // 0.1 s
  eq.process(st.data(), 4800);  // 0.2 s
  CHECK(near(eq.center(), 3772.0f, 1.0f));
  CHECK(near(eq.gain(), 0.30f, 1e-3f));

  // Disc census check for AudioMixerSnapshots.xml, GlobalDSPConfig.xml, AudioEngineConfig.xml.
  if (const char* game = std::getenv("SHIFT_GAME_ROOT")) {
    audio::SnapshotMatrix disc_m;
    CHECK(disc_m.load(game, &err));
    CHECK(disc_m.snapshot_count() == 39 && disc_m.category_count() == 21);
    CHECK(near(disc_m.dynamics().crash_threshold_mph, 50.0, 1e-3));
    CHECK(near(disc_m.dynamics().crash_drop_mph, 10.0, 1e-3));
    CHECK(near(disc_m.dynamics().top_speed_threshold, 0.70, 1e-3));
    CHECK(near(disc_m.dynamics().music_hp_cutoff, 200.0, 1e-3));

    audio::GlobalDspConfig gdsp;
    CHECK(gdsp.load(game, &err));
    CHECK(!gdsp.rewind.bypass && near(gdsp.rewind.gain, 0.30, 1e-3) && near(gdsp.rewind.center, 3772.0, 1.0));
    CHECK(!gdsp.countdown.bypass && near(gdsp.countdown.gain, 0.10, 1e-3));
    CHECK(gdsp.cockpit[0].bypass && gdsp.cockpit[1].bypass && gdsp.cockpit[2].bypass && gdsp.cockpit[3].bypass);

    audio::MasterDspConfig disc_mdsp;
    CHECK(disc_mdsp.load(game, &err));
    CHECK(near(disc_mdsp.compressor_ratio, 0.25, 1e-4) && near(disc_mdsp.compressor_ratio_tv, 0.15, 1e-4));
    CHECK(near(disc_mdsp.saturator_saturation, 0.90, 1e-4));
    std::printf("disc snapshots: %d snapshots, %d categories, compressor ratio %.2f/%.2f\n", disc_m.snapshot_count(),
                disc_m.category_count(), disc_mdsp.compressor_ratio, disc_mdsp.compressor_ratio_tv);
  }
}

// SPEC-AUDIO-EVENTS Parts 1 & 2: 6-point 3D rolloff, Doppler, BOV thresholds,
// and full base-disc 34-FEV / XML census checks.
void fev_and_event_config_checks() {
  // BOV thresholds.
  CHECK(audio::select_bov_variant(4.5) == audio::BovVariant::kNone);
  CHECK(audio::select_bov_variant(8.0) == audio::BovVariant::kLo);
  CHECK(audio::select_bov_variant(13.0) == audio::BovVariant::kLo);
  CHECK(audio::select_bov_variant(15.0) == audio::BovVariant::kHi);

  // 6-point 3D rolloff curve.
  audio::RolloffCurve rc{10.0f, 50.0f, 100.0f, 200.0f};
  const float dists[] = {0, 10, 30, 50, 100, 200, 250, 300};
  const float want[] = {1.0f, 1.0f, 0.675f, 0.35f, 0.10f, 0.05f, 0.025f, 0.0f};
  for (size_t i = 0; i < 8; ++i) CHECK(near(rc.eval(dists[i], 1.0f), want[i], 1e-5));
  CHECK(audio::replay_fov_rolloff_scale(0.35f, 0.87f, 0.45f) > 1.4f);

  // Doppler & stereo pan.
  const float dp_approach = audio::compute_doppler_pitch({0, 0, 0}, {0, 0, 0}, {0, 0, 50}, {0, 0, -34}, 1.0f);
  const float dp_recede = audio::compute_doppler_pitch({0, 0, 0}, {0, 0, 0}, {0, 0, 50}, {0, 0, 34}, 1.0f);
  CHECK(near(dp_approach, 340.0 / 306.0, 1e-4));
  CHECK(near(dp_recede, 340.0 / 374.0, 1e-4));
  float pl = 0, pr = 0;
  audio::compute_stereo_pan({0, 0, 0}, {1, 0, 0}, {10, 0, 0}, pl, pr);
  CHECK(pr > 0.99f && pl < 0.01f);

  const char* game = std::getenv("SHIFT_GAME_ROOT");
  if (!game) return;
  namespace fs = std::filesystem;
  const fs::path audio_root = fs::path(game) / "media" / "audio";
  int fev_files = 0, v40 = 0, v37 = 0;
  int total_events = 0, complex_ev = 0, simple_ev = 0;
  int total_sdefs = 0, total_entries = 0, wavetable = 0, silence = 0, programmer = 0;
  audio::FevProject ui_fev, gen_fev;
  std::string err;
  for (const auto& entry : fs::recursive_directory_iterator(audio_root)) {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".fev") continue;
    audio::FevProject proj;
    if (!audio::load_fev(entry.path(), proj, &err)) {
      std::printf("FAIL load_fev %s: %s\n", entry.path().string().c_str(), err.c_str());
      ++failures;
      continue;
    }
    ++fev_files;
    if (proj.version == 0x00400000u) ++v40;
    else if (proj.version == 0x00370000u) ++v37;
    total_events += int(proj.events.size());
    for (const auto& ev : proj.events) {
      if (ev.is_complex()) ++complex_ev;
      else if (ev.is_simple()) ++simple_ev;
    }
    total_sdefs += int(proj.sound_defs.size());
    for (const auto& sd : proj.sound_defs) {
      total_entries += int(sd.entries.size());
      for (const auto& e : sd.entries) {
        if (e.entry_type == 0) ++wavetable;
        else if (e.entry_type == 2) ++silence;
        else if (e.entry_type == 3) ++programmer;
      }
    }
    if (audio::iequals(proj.name, "UIInGame")) ui_fev = std::move(proj);
    else if (audio::iequals(proj.name, "General")) gen_fev = std::move(proj);
  }
  std::printf("disc fev census: %d files (%d v40, %d v37), %d events (%d complex, %d simple), %d sdefs, %d entries\n",
              fev_files, v40, v37, total_events, complex_ev, simple_ev, total_sdefs, total_entries);
  CHECK(fev_files == 34 && v40 == 32 && v37 == 2);
  CHECK(total_events == 1093 && complex_ev == 418 && simple_ev == 675);
  CHECK(total_sdefs == 1032 && total_entries == 4342);
  CHECK(wavetable == 4242 && silence == 96 && programmer == 4);

  // ui4audio.xml census + 100% resolution of non-Blank entries to UIInGame/General.
  audio::UiAudioMap uimap;
  CHECK(uimap.load(game, &err));
  CHECK(uimap.total_entries() == 762 && uimap.defaults.size() == 757);
  int non_blank = 0, resolved = 0, blank = 0;
  auto check_entry = [&](const audio::UiAudioEntry& e) {
    if (e.is_blank()) {
      ++blank;
    } else {
      ++non_blank;
      const std::string p = e.full_path();
      if (ui_fev.find_event(p) || gen_fev.find_event(p)) ++resolved;
    }
  };
  for (const auto& e : uimap.defaults) check_entry(e);
  for (const auto& [k, list] : uimap.overrides)
    for (const auto& e : list) check_entry(e);
  std::printf("disc ui4audio: %d total, %d/%d non-blank resolved, %d blank\n", uimap.total_entries(), resolved,
              non_blank, blank);
  CHECK(non_blank == 382 && resolved == 382 && blank == 380);

  // Tires.xml, SurfaceChanges.xml, CollisionData.xml, Backfire*, GlobalRolloffs*.
  audio::TiresConfig tc;
  CHECK(tc.load(game, &err));
  CHECK(tc.surfaces.size() == 42 && tc.changes.size() == 581);
  CHECK(tc.surface_change_event("Asphalt", "Brick") == "HardToHard");

  audio::CollisionAudioConfig cc;
  CHECK(cc.load(game, &err));
  CHECK(cc.pairs.size() == 615 && near(cc.ms_between_diff_collisions, 75.0, 1e-3));
  CHECK(cc.find_pair("Asphalt", "CarAluminum") != nullptr);

  audio::BackfireConfig bc;
  CHECK(bc.load(game, &err));
  CHECK(bc.levels[0].silence_weighting == 100 && bc.levels[3].silence_weighting == 80);

  audio::RolloffConfig rg, rt;
  CHECK(rg.load(game, false, false, &err) && rt.load(game, true, false, &err));
  CHECK(near(rg.chassis_exhaust_l.min_dist, 10.0, 1e-3));
}

void sfx_engine_checks() {
  auto make_tone = [](double hz, int frames, int rate = 48000) {
    audio::PcmSample s;
    s.rate = rate;
    s.data.resize(size_t(frames));
    for (int i = 0; i < frames; ++i)
      s.data[size_t(i)] = float(0.6 * std::sin(2.0 * 3.14159265358979323846 * hz * double(i) / double(rate)));
    return s;
  };
  auto bus_rms = [](const std::vector<float>& buf, int ch = -1) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = 0; i + 1 < buf.size(); i += 2) {
      if (ch == 0) { acc += double(buf[i]) * buf[i]; ++n; }
      else if (ch == 1) { acc += double(buf[i + 1]) * buf[i + 1]; ++n; }
      else { acc += 0.5 * (double(buf[i]) * buf[i] + double(buf[i + 1]) * buf[i + 1]); ++n; }
    }
    return n > 0 ? std::sqrt(acc / double(n)) : 0.0;
  };

  audio::SfxEngine sfx(48000);
  for (int i = 0; i < int(audio::SfxSoundId::kCount); ++i)
    sfx.set_sample(audio::SfxSoundId(i), make_tone(220.0 + 30.0 * i, 9600));

  // Configure deterministic backfire on overrun (silence_weighting = 0, fast timer).
  audio::BackfireConfig bcfg;
  bcfg.levels[3].min_norm_rpm = 0.2;
  bcfg.levels[3].max_norm_rpm = 0.9;
  bcfg.levels[3].min_time_ms = 50.0;
  bcfg.levels[3].max_time_ms = 60.0;
  bcfg.levels[3].silence_weighting = 0.0;
  sfx.set_backfire_config(bcfg);

  // Synthetic EnginePlayer to verify Backfire -> Expander coupling.
  audio::EngineSound es;
  es.audio_rpm_idle = 800.0;
  es.audio_rpm_redline = 7000.0;
  audio::Group grp;
  grp.name = "ExhaustL";
  grp.expander.present = true;
  grp.expander.attack_ms = 5.0;
  grp.expander.hold_ms = 50.0;
  grp.expander.release_ms = 200.0;
  grp.expander.max_gain.present = true;
  grp.expander.max_gain.y = {2.0, 2.0, 2.0};
  grp.loops = {loop(500, 3000, 9000)};
  es.groups.push_back(grp);
  audio::PcmSample eng_tone = make_tone(150.0, 4800);
  audio::EnginePlayer ep(es, {{&eng_tone}}, 48000, 800.0, 7000.0);
  ep.set({4500.0, 0.0, 0.0, 0.0});

  constexpr int kFrames = 2400;  // 50 ms
  std::array<std::vector<float>, audio::kBuses> bufs;
  float* ptrs[audio::kBuses] = {};
  auto clear_bufs = [&]() {
    for (int b = 0; b < audio::kBuses; ++b) {
      bufs[size_t(b)].assign(kFrames * 2, 0.0f);
      ptrs[b] = bufs[size_t(b)].data();
    }
  };

  audio::VehicleAudioState vs{};
  vs.speed_mph = 110.0f;
  vs.rpm = 4500.0f;
  vs.idle_rpm = 800.0f;
  vs.redline_rpm = 7000.0f;
  vs.throttle = 0.0f;  // overrun for backfire
  vs.forced_induction = 1;
  vs.turbo_level = 1;
  vs.turbo_norm_rpm = 0.8f;
  vs.turbo_psi = 16.0f;
  vs.bov_open = true;
  vs.shifting = true;
  vs.trans_level = 2;
  vs.backfire_level = 3;
  for (auto& w : vs.wheels) {
    w.surface_group = 0;
    w.slip_combined = 0.7f;
    w.load = 1.0f;
  }
  vs.listener_pos = {0, 0, 0};
  vs.listener_right = {1, 0, 0};
  // Opponent car 12 m to the right -> right channel must be louder than left.
  vs.other_cars[0].active = true;
  vs.other_cars[0].pos = {12.0f, 0.0f, 0.0f};
  vs.other_cars[0].vel = {0.0f, 0.0f, 30.0f};
  vs.other_cars[0].rpm = 5500.0f;

  // Render enough blocks to cross backfire timer (~500 ms initial timer).
  double max_wind = 0, max_tyres = 0, max_eng = 0, max_trans = 0, max_turbo = 0, max_backfire = 0;
  double max_other_l = 0, max_other_r = 0;
  double max_exp = 0.0;
  for (int blk = 0; blk < 14; ++blk) {
    clear_bufs();
    if (blk == 2) vs.wheels[0].surface_group = 1;  // triggers DirtToHard one-shot
    if (blk == 3) vs.shifting = false;             // triggers ShiftComplete one-shot
    sfx.render(vs, ptrs, kFrames, &ep);
    std::vector<float> tmp(kFrames * 2, 0.0f);
    ep.render(tmp.data(), kFrames);
    max_exp = std::max(max_exp, ep.expander_state(0));
    max_wind = std::max(max_wind, bus_rms(bufs[int(audio::Bus::kWind)]));
    max_tyres = std::max(max_tyres, bus_rms(bufs[int(audio::Bus::kTyres)]));
    max_eng = std::max(max_eng, bus_rms(bufs[int(audio::Bus::kEngine)]));
    max_trans = std::max(max_trans, bus_rms(bufs[int(audio::Bus::kTransmission)]));
    max_turbo = std::max(max_turbo, bus_rms(bufs[int(audio::Bus::kTurbo)]));
    max_backfire = std::max(max_backfire, bus_rms(bufs[int(audio::Bus::kBackfire)]));
    max_other_l = std::max(max_other_l, bus_rms(bufs[int(audio::Bus::kOtherCars)], 0));
    max_other_r = std::max(max_other_r, bus_rms(bufs[int(audio::Bus::kOtherCars)], 1));
  }
  CHECK(max_wind > 0.01);
  CHECK(max_tyres > 0.05);
  CHECK(max_eng > 0.01);
  CHECK(max_trans > 0.02);
  CHECK(max_turbo > 0.05);
  CHECK(max_backfire > 0.05);
  CHECK(max_other_r > max_other_l * 1.3);
  CHECK(max_exp > 0.1);  // Backfire triggered EnginePlayer::Expander!

  // Collision 75 ms rate limit check.
  audio::SfxEngine csfx(48000);
  csfx.set_sample(audio::SfxSoundId::kImpact3d, make_tone(180.0, 2400));
  csfx.set_sample(audio::SfxSoundId::kImpactLfe, make_tone(60.0, 2400));
  csfx.set_sample(audio::SfxSoundId::kUiAccept, make_tone(880.0, 2400));
  audio::CollisionHit hit{};
  hit.rel_speed_mps = 12.0f;
  hit.pos = {2.0f, 0.0f, 0.0f};
  csfx.trigger_collision(hit, vs);
  const int after_first = csfx.active_one_shots();
  CHECK(after_first == 2);  // Impact3d + ImpactLfe
  // Immediate second collision (< 75 ms) must be rate-limited.
  csfx.trigger_collision(hit, vs);
  CHECK(csfx.active_one_shots() == 2);
  // Advance 100 ms (> 75 ms) so previous shots finish and gate re-opens.
  for (int i = 0; i < 2; ++i) {
    clear_bufs();
    csfx.render(vs, ptrs, kFrames);
  }
  CHECK(csfx.active_one_shots() == 0);
  csfx.trigger_collision(hit, vs);
  CHECK(csfx.active_one_shots() == 2);

  // UI trigger renders into Bus::kUi.
  csfx.trigger_ui(audio::UiCue::kAccept);
  clear_bufs();
  csfx.render(vs, ptrs, kFrames);
  CHECK(bus_rms(bufs[int(audio::Bus::kUi)]) > 0.05);

  // Full AudioEngine 8-bus + SfxEngine offline mix check.
  audio::AudioEngine eng(48000);
  auto sfx2 = std::make_unique<audio::SfxEngine>(48000);
  sfx2->set_sample(audio::SfxSoundId::kWind, make_tone(300.0, 4800));
  sfx2->set_sample(audio::SfxSoundId::kRewindTapeStart, make_tone(660.0, 4800));
  eng.set_sfx_engine(std::move(sfx2));
  eng.set_vehicle_state(vs);
  eng.trigger_ui(audio::UiCue::kRewindTapeStart);
  std::vector<float> mixed(kFrames * 2, 0.0f);
  eng.render_offline(mixed.data(), kFrames);
  CHECK(bus_rms(mixed) > 0.02);

  // Pillar 5 P0-13: 4-wheel stereo-panned tires (left-only slip pans left,
  // right-only slip pans right), multi-tier turbo/trans, per-AI variant loops,
  // continuous scrape & rewind loops, and near-miss Whoosh3D.
  {
    audio::SfxEngine psfx(48000);
    psfx.set_sample(audio::SfxSoundId::kTireSkidOnRoad, make_tone(750.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kTireSkidOffRoad, make_tone(320.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kTransWhineL2, make_tone(920.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kTurboSpoolL3, make_tone(1800.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kOtherCarLoop2, make_tone(190.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kScrapeLoop, make_tone(420.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kRewindLoop, make_tone(520.0, 9600));
    psfx.set_sample(audio::SfxSoundId::kWhoosh3d, make_tone(260.0, 14400));

    audio::VehicleAudioState st{};
    st.speed_mph = 70.0f;
    st.rpm = 5500.0f;
    st.idle_rpm = 800.0f;
    st.redline_rpm = 7500.0f;
    st.trans_level = 2;
    st.forced_induction = 1;
    st.turbo_level = 3;
    st.turbo_norm_rpm = 0.85f;
    st.scraping = true;
    st.scrape_intensity = 0.8f;
    st.rewinding = true;
    // Left-front wheel slipping heavily on asphalt; right wheels zero slip.
    st.wheels[0] = {0, 0.9f, 900.0f, 1.0f, 0.0f};
    st.wheels[1] = {0, 0.0f, 900.0f, 1.0f, 0.0f};
    st.wheels[2] = {1, 0.0f, 900.0f, 1.0f, 0.0f};
    st.wheels[3] = {1, 0.0f, 900.0f, 1.0f, 0.0f};
    // Fast oncoming opponent passing closely on the right -> Whoosh3D + variant 2 loop.
    st.listener_pos = {0, 0, 0};
    st.listener_vel = {0, 0, 35.0f};
    st.listener_right = {1, 0, 0};
    st.other_cars[0].active = true;
    st.other_cars[0].variant = 2;
    st.other_cars[0].pos = {4.0f, 0.0f, 2.0f};
    st.other_cars[0].vel = {0.0f, 0.0f, -25.0f};
    st.other_cars[0].rpm = 4800.0f;

    clear_bufs();
    psfx.render(st, ptrs, kFrames);
    const double tyre_l = bus_rms(bufs[int(audio::Bus::kTyres)], 0);
    const double tyre_r = bus_rms(bufs[int(audio::Bus::kTyres)], 1);
    CHECK(tyre_l > tyre_r * 1.25);                                    // Left-front wheel skid localized to left channel
    CHECK(bus_rms(bufs[int(audio::Bus::kTransmission)]) > 0.02);      // Trans L2 on dedicated Transmission bus
    CHECK(bus_rms(bufs[int(audio::Bus::kTurbo)]) > 0.02);             // Turbo L3 on dedicated Turbo bus
    CHECK(bus_rms(bufs[int(audio::Bus::kCollision)]) > 0.02);         // Continuous Scrape3D loop on Collision bus
    CHECK(bus_rms(bufs[int(audio::Bus::kUi)]) > 0.02);                // Continuous Mulligan rewind loop
    CHECK(bus_rms(bufs[int(audio::Bus::kOtherCars)]) > 0.02);         // Variant 2 opponent + Whoosh3D
    CHECK(psfx.active_one_shots() >= 1);                      // Whoosh3D spawned on close pass
  }

  // Pillar 5 P0-14: LoadPEQ parses Active="1" on <LoadPEQ> (not on <PosLoad>/<NegLoad>),
  // and SurroundPan preserves relative loudness without normalizing max(L,R) to 1.0.
  {
    const std::string eng_xml = R"(
<Engine>
  <HarmonicTunings>
    <EmissionGroup2>
      <LoadPEQ Active="1">
        <PosLoad>
          <Gain>
            <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
            <ThreePointCurve x0="0" y0="1.0" x1="0.5" y1="1.25" x2="1" y2="1.5"/>
          </Gain>
          <CenterFrequency>
            <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
            <ThreePointCurve x0="0" y0="600" x1="0.5" y1="900" x2="1" y2="1200"/>
          </CenterFrequency>
          <Bandwidth>
            <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
            <ThreePointCurve x0="0" y0="1.0" x1="0.5" y1="1.1" x2="1" y2="1.2"/>
          </Bandwidth>
        </PosLoad>
        <NegLoad>
          <Gain>
            <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
            <ThreePointCurve x0="0" y0="0.8" x1="0.5" y1="0.85" x2="1" y2="0.9"/>
          </Gain>
          <CenterFrequency>
            <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
            <ThreePointCurve x0="0" y0="400" x1="0.5" y1="550" x2="1" y2="700"/>
          </CenterFrequency>
          <Bandwidth>
            <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
            <ThreePointCurve x0="0" y0="1.0" x1="0.5" y1="1.0" x2="1" y2="1.0"/>
          </Bandwidth>
        </NegLoad>
      </LoadPEQ>
    </EmissionGroup2>
  </HarmonicTunings>
</Engine>)";
    const std::string ht_xml = R"(
<Soundbank name="test_exh.fsb">
  <ExhaustL>
    <Loop wavebankindex="0" rpm_min="800" rpm_max="7000" rpm_sample="3000" volume="1" pitch="1" pitchMin="0.5" pitchMax="2.0"/>
  </ExhaustL>
</Soundbank>)";
    const std::string model_xml = R"(
<ModelTuning>
  <Listener>
    <Basic>
      <Follow>
        <EmissionGroupVolumes ExhaustL="1.0"/>
        <SurroundPanExhaustL Left="0.4" Right="0.4" Center="0.0" LeftRear="0.0" RightRear="0.0"/>
      </Follow>
    </Basic>
  </Listener>
</ModelTuning>)";
    audio::XmlNode eng_n, ht_n, mod_n;
    std::string xerr;
    CHECK(audio::parse_xml(eng_xml, eng_n, &xerr));
    CHECK(audio::parse_xml(ht_xml, ht_n, &xerr));
    CHECK(audio::parse_xml(model_xml, mod_n, &xerr));
    audio::EngineSound parsed;
    CHECK(audio::read_harmonic(eng_n, ht_n, "Exhaust", parsed, &xerr));
    CHECK(!parsed.groups.empty());
    CHECK(parsed.groups[0].load_peq.active);
    CHECK(parsed.groups[0].load_peq.pos_load.active);
    CHECK(parsed.groups[0].load_peq.neg_load.active);
    audio::read_model_tuning(mod_n, parsed);
    CHECK(near(parsed.groups[0].pan_left[int(audio::View::kFollow)], 0.4, 1e-6));
    CHECK(near(parsed.groups[0].pan_right[int(audio::View::kFollow)], 0.4, 1e-6));
  }

  // Pillar 5 P0-14: AudioEngine matrix mode does NOT double-stack legacy SnapshotMixer
  // gains on Paused (music ducks to 0.25, not 0.0625) or Menu (stays audible, not muted to 0).
  {
    const std::string snap_xml = R"(
<MixSnapshots>
  <FreeRoam group="Gameplay">
    <FocusCar active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
    <CarWind active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="1.0" fadetime="0" returntime="0" returnfadetime="0"/>
  </FreeRoam>
  <Paused group="Gameplay">
    <FocusCar active="1" destvolume="0.0" fadetime="0" returntime="0" returnfadetime="0"/>
    <CarWind active="1" destvolume="0.0" fadetime="0" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="0.25" fadetime="0" returntime="0" returnfadetime="0"/>
  </Paused>
  <UIExterior group="Gameplay">
    <FocusCar active="1" destvolume="0.2" fadetime="0" returntime="0" returnfadetime="0"/>
    <CarWind active="1" destvolume="0.5" fadetime="0" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="0.6" fadetime="0" returntime="0" returnfadetime="0"/>
  </UIExterior>
</MixSnapshots>)";
    audio::AudioEngine meng(48000);
    std::string serr;
    CHECK(meng.load_snapshot_matrix_xml(snap_xml, &serr));
    meng.set_full_dynamic_range(true);
    // Even if legacy Paused/Menu snapshots were activated, matrix mode must not double-duck/mute.
    meng.set_snapshot(meng.find_snapshot("Paused"), true);
    meng.set_snapshot(meng.find_snapshot("Menu"), true);
    meng.activate_matrix_snapshot("UIExterior");
    auto sfx_m = std::make_unique<audio::SfxEngine>(48000);
    sfx_m->set_sample(audio::SfxSoundId::kWind, make_tone(400.0, 9600));
    meng.set_sfx_engine(std::move(sfx_m));
    audio::VehicleAudioState mvs{};
    mvs.speed_mph = 120.0f;
    meng.set_vehicle_state(mvs);
    std::vector<float> mout(4800 * 2, 0.0f);
    for (int i = 0; i < 4; ++i) meng.render_offline(mout.data(), 4800);
    CHECK(near(meng.matrix_bus_gain(audio::Bus::kMusic), 0.6, 1e-4));
    CHECK(bus_rms(mout) > 0.01);  // Not hard-muted by legacy Menu/Paused snapshot!
  }

  // P0-2: Concurrent SfxEngine stress test — simultaneous worker_, burble_worker_,
  // set_sample() producers, and real-time audio render() consumer with zero races.
  {
    namespace fs = std::filesystem;
    const fs::path tmp_root = fs::temp_directory_path() / "shift-sfx-race-test";
    const fs::path tmp_cache = tmp_root / "cache";
    std::error_code ec;
    fs::remove_all(tmp_root, ec);
    const fs::path burble_dir = tmp_root / "media" / "audio" / "cars" / "Engines" / "Soundbanks" / "Burbles";
    const fs::path sfx_cache = tmp_cache / "sfx";
    fs::create_directories(burble_dir, ec);
    fs::create_directories(sfx_cache, ec);

    // Create two synthetic FSB4 banks with 4 subsounds each and pre-seed their .s16 cache files
    // so both worker_ and burble_worker_ push rapidly and concurrently without waiting on ffmpeg.
    auto make_mock_fsb4 = [&](const fs::path& p, uint32_t subs) {
      const size_t data_off = (48 + subs * 80 + 31) & ~size_t(31);
      std::vector<uint8_t> bank(data_off + subs * 64, 0);
      auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) bank[at + size_t(i)] = uint8_t(v >> (8 * i));
      };
      std::copy_n("FSB4", 4, bank.begin());
      put32(4, subs);
      put32(8, subs * 80);
      put32(12, subs * 64);
      for (uint32_t s = 0; s < subs; ++s) {
        const size_t off = 48 + s * 80;
        bank[off] = 80;
        put32(off + 32, 64);
        put32(off + 36, 64);
        put32(off + 48, 0x01000000);
        put32(off + 52, 48000);
        bank[off + 62] = 1;
      }
      std::ofstream f(p, std::ios::binary);
      f.write(reinterpret_cast<const char*>(bank.data()), std::streamsize(bank.size()));
    };
    auto write_s16 = [&](const std::string& stem, uint32_t sub, float freq) {
      const fs::path cp = sfx_cache / (stem + "." + std::to_string(sub) + ".s16");
      std::ofstream f(cp, std::ios::binary | std::ios::trunc);
      const int32_t rate = 48000;
      const uint32_t frames = 512;
      f.write(reinterpret_cast<const char*>(&rate), 4);
      f.write(reinterpret_cast<const char*>(&frames), 4);
      std::vector<int16_t> pcm(frames);
      for (uint32_t i = 0; i < frames; ++i) {
        pcm[i] = int16_t(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979 * freq * double(i) / 48000.0)));
      }
      f.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(frames * 2));
    };

    make_mock_fsb4(burble_dir / "burble_a.fsb", 4);
    make_mock_fsb4(burble_dir / "burble_b.fsb", 4);
    for (uint32_t i = 0; i < 4; ++i) {
      write_s16("burble_a", i, 180.0f + 40.0f * float(i));
      write_s16("burble_b", i, 260.0f + 50.0f * float(i));
    }

    audio::SfxEngine stress_sfx(48000);
    std::atomic<bool> done{false};
    std::atomic<int> bad_samples{0};

    // Audio consumer thread: continuously renders and triggers UI/collisions.
    std::thread consumer([&]() {
      std::vector<float> eng(256 * 2, 0.0f);
      std::vector<float> wind(256 * 2, 0.0f);
      std::vector<float> ui(256 * 2, 0.0f);
      float* buses[audio::kBuses]{};
      buses[int(audio::Bus::kEngine)] = eng.data();
      buses[int(audio::Bus::kWind)] = wind.data();
      buses[int(audio::Bus::kUi)] = ui.data();
      audio::VehicleAudioState vs{};
      vs.speed_mph = 95.0f;
      vs.rpm = 4800.0f;
      vs.throttle = 0.0f;
      vs.backfire_level = 3;
      while (!done.load(std::memory_order_acquire)) {
        std::fill(eng.begin(), eng.end(), 0.0f);
        std::fill(wind.begin(), wind.end(), 0.0f);
        std::fill(ui.begin(), ui.end(), 0.0f);
        stress_sfx.render(vs, buses, 256, nullptr);
        for (float v : eng)
          if (!std::isfinite(v)) bad_samples.fetch_add(1, std::memory_order_relaxed);
      }
    });

    // Producer 1: repeatedly reloads main SFX bank (worker_ -> loaded_q_).
    std::thread p_main([&]() {
      for (int i = 0; i < 20; ++i) {
        stress_sfx.load(tmp_root, tmp_cache, (i & 1) ? "burble_a.fsb" : "burble_b.fsb", nullptr);
      }
    });
    // Producer 2: repeatedly reloads burble bank (burble_worker_ -> burble_loaded_q_).
    std::thread p_burble([&]() {
      for (int i = 0; i < 40; ++i) {
        stress_sfx.reload_burble_bank(tmp_root, tmp_cache, (i & 1) ? "burble_b.fsb" : "burble_a.fsb");
      }
    });
    // Producer 3: pushes direct samples concurrently (set_sample -> direct_loaded_q_).
    std::thread p_direct([&]() {
      for (int i = 0; i < 80; ++i) {
        stress_sfx.set_sample(audio::SfxSoundId::kWind, make_tone(200.0 + double(i), 256));
        stress_sfx.set_sample(audio::SfxSoundId::kUiAccept, make_tone(600.0 + double(i), 256));
      }
    });

    p_main.join();
    p_burble.join();
    p_direct.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    done.store(true, std::memory_order_release);
    consumer.join();

    // Final drain and verification.
    float* null_buses[audio::kBuses]{};
    audio::VehicleAudioState vs{};
    stress_sfx.render(vs, null_buses, 64, nullptr);
    CHECK(bad_samples.load() == 0);
    CHECK(stress_sfx.loaded_sample_count() >= 4);
    fs::remove_all(tmp_root, ec);
  }

  // P0-6: Airborne Showcase 3D aircraft audio (procedural fallback + decoded PCM + Doppler).
  {
    audio::SfxEngine sc_sfx(48000);
    std::vector<float> other(4800 * 2, 0.0f);
    float* buses[audio::kBuses]{};
    buses[int(audio::Bus::kOtherCars)] = other.data();
    for (uint8_t kind = 0; kind <= 4; ++kind) {
      std::fill(other.begin(), other.end(), 0.0f);
      audio::VehicleAudioState vs{};
      vs.listener_pos = {0.0f, 0.0f, 0.0f};
      vs.listener_right = {1.0f, 0.0f, 0.0f};
      vs.showcase.active = true;
      vs.showcase.kind = kind;
      vs.showcase.pos = {15.0f, 20.0f, 25.0f};
      vs.showcase.vel = {0.0f, -2.0f, -55.0f};
      vs.showcase.speed_mps = 55.0f;
      vs.showcase.bank = 0.4f;
      vs.showcase.pitch = 0.1f;
      sc_sfx.render(vs, buses, 4800, nullptr);
      CHECK(bus_rms(other) > 0.005);
    }
    // Also verify decoded Triggerable_Events PCM takes precedence when loaded.
    sc_sfx.set_sample(audio::SfxSoundId::kShowcaseHeli, make_tone(320.0, 9600));
    std::fill(other.begin(), other.end(), 0.0f);
    audio::VehicleAudioState vs{};
    vs.showcase.active = true;
    vs.showcase.kind = 4;
    vs.showcase.pos = {5.0f, 15.0f, 10.0f};
    vs.showcase.speed_mps = 40.0f;
    sc_sfx.render(vs, buses, 4800, nullptr);
    CHECK(bus_rms(other) > 0.02);
  }

  // P0-8: Android procedural SFX fallback populates rich non-silent SFX without external ffmpeg.
  {
    audio::SfxEngine droid_sfx(48000);
    droid_sfx.populate_procedural_fallbacks();
    std::vector<float> wind(2400 * 2, 0.0f), tyres(2400 * 2, 0.0f), eng(2400 * 2, 0.0f), ui(2400 * 2, 0.0f);
    float* buses[audio::kBuses]{};
    buses[int(audio::Bus::kWind)] = wind.data();
    buses[int(audio::Bus::kTyres)] = tyres.data();
    buses[int(audio::Bus::kEngine)] = eng.data();
    buses[int(audio::Bus::kUi)] = ui.data();
    audio::VehicleAudioState vs{};
    vs.speed_mph = 110.0f;
    vs.rpm = 5400.0f;
    vs.throttle = 0.8f;
    vs.forced_induction = 1;
    vs.turbo_norm_rpm = 0.9f;
    for (auto& w : vs.wheels) {
      w.load = 1.0f;
      w.slip_combined = 0.6f;
    }
    droid_sfx.trigger_ui(audio::UiCue::kAccept);
    droid_sfx.render(vs, buses, 2400, nullptr);
    CHECK(droid_sfx.loaded_sample_count() >= 40);
    CHECK(bus_rms(wind) > 0.005);
    CHECK(bus_rms(tyres) > 0.005);
    CHECK(bus_rms(eng) > 0.005);
    CHECK(bus_rms(ui) > 0.005);
  }

  // Round 3 P1-6: 22-category audio snapshot bus separation + ElectricEngine/Reflections/ActiveAero FEV slots.
  {
    CHECK(audio::category_to_bus(audio::Category::kFocusCar) == audio::Bus::kEngine);
    CHECK(audio::category_to_bus(audio::Category::kTransmission) == audio::Bus::kTransmission);
    CHECK(audio::category_to_bus(audio::Category::kSuperCharger) == audio::Bus::kSupercharger);
    CHECK(audio::category_to_bus(audio::Category::kTurbo) == audio::Bus::kTurbo);
    CHECK(audio::category_to_bus(audio::Category::kBackfire) == audio::Bus::kBackfire);
    CHECK(audio::category_to_bus(audio::Category::kSuspension) == audio::Bus::kSuspension);
    CHECK(audio::category_to_bus(audio::Category::kCollision) == audio::Bus::kCollision);
    CHECK(audio::category_to_bus(audio::Category::kSatNav) == audio::Bus::kSatNav);
    CHECK(audio::category_to_bus(audio::Category::kRallyCoDriver) == audio::Bus::kRallyCoDriver);

    const char* snap_xml =
        "<AudioMixerSnapshots>"
        "  <FreeRoam group=\"Gameplay\">"
        "    <FocusCar active=\"1\" destvolume=\"1.0\" fadetime=\"0.0\"/>"
        "    <Transmission active=\"1\" destvolume=\"0.4\" fadetime=\"0.0\"/>"
        "    <Turbo active=\"1\" destvolume=\"0.8\" fadetime=\"0.0\"/>"
        "    <Backfire active=\"1\" destvolume=\"0.9\" fadetime=\"0.0\"/>"
        "    <Suspension active=\"1\" destvolume=\"0.5\" fadetime=\"0.0\"/>"
        "  </FreeRoam>"
        "</AudioMixerSnapshots>";
    audio::SnapshotMatrix mat;
    CHECK(mat.parse(snap_xml));
    CHECK(near(mat.bus_gain(audio::Bus::kEngine), 1.0, 1e-4));
    CHECK(near(mat.bus_gain(audio::Bus::kTransmission), 0.4, 1e-4));
    CHECK(near(mat.bus_gain(audio::Bus::kTurbo), 0.8, 1e-4));
    CHECK(near(mat.bus_gain(audio::Bus::kBackfire), 0.9, 1e-4));
    CHECK(near(mat.bus_gain(audio::Bus::kSuspension), 0.5, 1e-4));

    // Verify EV motor (ElectricEngine.fev), Roadside Reflections (Reflections.fev), and ActiveAero (ActiveAero.fev).
    audio::SfxEngine ev_sfx(48000);
    ev_sfx.populate_procedural_fallbacks();
    std::array<std::vector<float>, audio::kBuses> bufs;
    float* ptrs[audio::kBuses]{};
    for (int b = 0; b < audio::kBuses; ++b) {
      bufs[size_t(b)].assign(2400 * 2, 0.0f);
      ptrs[b] = bufs[size_t(b)].data();
    }
    audio::VehicleAudioState vs{};
    vs.is_electric = true;
    vs.speed_mph = 85.0f;
    vs.rpm = 6000.0f;
    vs.throttle = 0.85f;
    vs.reflection_proximity = 0.8f;
    vs.active_aero_deploy = 1.0f;
    ev_sfx.render(vs, ptrs, 2400, nullptr);
    CHECK(bus_rms(bufs[int(audio::Bus::kEngine)]) > 0.01);
    CHECK(bus_rms(bufs[int(audio::Bus::kAmbience)]) > 0.005);
    CHECK(bus_rms(bufs[int(audio::Bus::kWind)]) > 0.01);
  }

  // Optional disc smoke check when SHIFT_GAME_ROOT is set.
  const char* game_env = std::getenv("SHIFT_GAME_ROOT");
  if (game_env && *game_env) {
    namespace fs = std::filesystem;
    const fs::path game(game_env);
    if (fs::is_directory(game / "media" / "audio")) {
      audio::SfxEngine disc_sfx(48000);
      std::string note;
      CHECK(disc_sfx.load(game, {}, {}, &note));
      std::printf("disc %s\n", note.c_str());
      CHECK(note.find("queued ") != std::string::npos);
    }
  }
}

// Simulates a 250 ms main-thread & stream-underrun hitch followed by 2 seconds
// of full engine + SFX + reverb + biquad DSP audio, asserting zero NaNs, zero
// clipping (|s| <= 1.0), smooth zero-click fade-in on underrun recovery, and
// bounded per-sample derivative (zero crackle spikes).
void hitch_and_underrun_recovery_checks() {
  constexpr int kRate = 48000;
  constexpr int kBlock = 480;  // 10 ms

  // Build an engine sound with active resonant LP + PEQ + LoadPEQ biquads.
  audio::EngineSound sound;
  sound.audio_rpm_idle = 900.0;
  sound.audio_rpm_redline = 8000.0;
  audio::Group g;
  g.name = "ExhaustL";
  g.loops = {loop(500, 1500, 3500), loop(2500, 4500, 6500), loop(5500, 7200, 32000)};
  g.lowpass = true;
  g.lowpass_cutoff.present = true;
  g.lowpass_cutoff.rpm = 1.0;
  g.lowpass_cutoff.x = {0.0, 0.5, 1.0};
  g.lowpass_cutoff.y = {450.0, 2200.0, 7500.0};
  g.lowpass_resonance.present = true;
  g.lowpass_resonance.throttle = 1.0;
  g.lowpass_resonance.x = {0.0, 0.5, 1.0};
  g.lowpass_resonance.y = {0.8, 3.0, 6.0};
  g.peq.active = true;
  g.peq.gain.present = true;
  g.peq.gain.rpm = 1.0;
  g.peq.gain.x = {0.0, 0.5, 1.0};
  g.peq.gain.y = {1.2, 1.8, 2.2};
  g.peq.center_freq.present = true;
  g.peq.center_freq.rpm = 1.0;
  g.peq.center_freq.x = {0.0, 0.5, 1.0};
  g.peq.center_freq.y = {220.0, 800.0, 2400.0};
  g.peq.bandwidth.present = true;
  g.peq.bandwidth.x = {0.0, 0.5, 1.0};
  g.peq.bandwidth.y = {0.8, 1.0, 1.2};
  sound.groups.push_back(g);

  audio::PcmSample loop_pcm;
  loop_pcm.rate = kRate;
  loop_pcm.data.resize(4800);
  for (size_t i = 0; i < loop_pcm.data.size(); ++i) {
    const double ph = 2.0 * 3.14159265358979 * 110.0 * double(i) / double(kRate);
    loop_pcm.data[i] = float(0.45 * std::sin(ph) + 0.25 * std::sin(2.0 * ph) + 0.12 * std::sin(3.0 * ph));
  }

  // Part A: Deterministic offline 250 ms hitch (abrupt RPM/throttle/NaN jump)
  // followed by 2 full seconds (96,000 frames) of audio rendering.
  {
    audio::AudioEngine eng(kRate);
    auto ep = std::make_unique<audio::EnginePlayer>(
        sound, std::vector<std::vector<const audio::PcmSample*>>{{&loop_pcm, &loop_pcm, &loop_pcm}}, kRate, 900.0,
        8000.0);
    ep->master = 0.7;
    eng.set_engine_player(std::move(ep));
    auto sfx = std::make_unique<audio::SfxEngine>(kRate);
    sfx->populate_procedural_fallbacks();
    eng.set_sfx_engine(std::move(sfx));
    audio::AcousticsSample ac{};
    ac.reverb.near_wet = 0.4f;
    ac.reverb.decay_time_s = 1.8f;
    eng.set_acoustics_sample(ac);

    // Prime steady-state cruise audio (200 ms).
    audio::EnginePlayer::Update u{2600.0, 0.35, 0.3, 0.0, audio::View::kFollow};
    audio::VehicleAudioState vs{};
    vs.speed_mph = 70.0f;
    vs.rpm = 2600.0f;
    vs.idle_rpm = 900.0f;
    vs.redline_rpm = 8000.0f;
    vs.throttle = 0.35f;
    vs.forced_induction = 1;
    vs.turbo_level = 1;
    vs.turbo_norm_rpm = 0.5f;
    for (auto& w : vs.wheels) {
      w.load = 1.0f;
      w.slip_combined = 0.1f;
    }
    eng.set_engine_input(u);
    eng.set_vehicle_state(vs);
    std::vector<float> warmup(size_t(kRate / 5) * 2, 0.0f);
    eng.render_offline(warmup.data(), kRate / 5);

    // Simulate a 250 ms hitch with corrupt/NaN physics transient followed by
    // abrupt full-throttle redline + high-slip recovery.
    audio::EnginePlayer::Update bad_u{std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::infinity(), -5.0, 99.0, audio::View::kCockpit};
    audio::VehicleAudioState bad_vs = vs;
    bad_vs.speed_mph = std::numeric_limits<float>::quiet_NaN();
    bad_vs.rpm = std::numeric_limits<float>::infinity();
    bad_vs.wheels[0].slip_combined = std::numeric_limits<float>::quiet_NaN();
    eng.set_engine_input(bad_u);
    eng.set_vehicle_state(bad_vs);
    std::vector<float> hitch_blk(size_t(kBlock) * 2, 0.0f);
    eng.render_offline(hitch_blk.data(), kBlock);

    // Immediately snap to post-hitch aggressive driving and render 2 full seconds.
    u = {7400.0, 1.0, 0.95, 0.0, audio::View::kFollow};
    vs.speed_mph = 145.0f;
    vs.rpm = 7400.0f;
    vs.throttle = 1.0f;
    vs.turbo_norm_rpm = 1.0f;
    vs.wheels[2].slip_combined = 0.85f;
    vs.wheels[3].slip_combined = 0.85f;
    eng.set_engine_input(u);
    eng.set_vehicle_state(vs);

    constexpr int kPostFrames = kRate * 2;  // 2.0 seconds = 96,000 stereo frames
    std::vector<float> post(size_t(kPostFrames) * 2, 0.0f);
    for (int off = 0; off < kPostFrames; off += kBlock) {
      if (off == kRate / 2) {
        u = {3800.0, 0.0, 0.0, -0.7, audio::View::kCockpit};
        vs.rpm = 3800.0f;
        vs.throttle = 0.0f;
        vs.backfire_level = 2;
        eng.set_engine_input(u);
        eng.set_vehicle_state(vs);
      }
      eng.render_offline(post.data() + size_t(off) * 2, kBlock);
    }

    int non_finite = 0;
    int clipped = 0;
    float max_abs = 0.0f;
    float max_deriv = 0.0f;
    double energy = 0.0;
    for (float s : hitch_blk) {
      if (!std::isfinite(s)) ++non_finite;
      if (std::fabs(s) > 1.0001f) ++clipped;
    }
    for (int i = 0; i < kPostFrames; ++i) {
      for (int ch = 0; ch < 2; ++ch) {
        const float s = post[size_t(i) * 2 + size_t(ch)];
        if (!std::isfinite(s)) ++non_finite;
        const float a = std::fabs(s);
        if (a > 1.0001f) ++clipped;
        max_abs = std::max(max_abs, a);
        energy += double(s) * double(s);
        if (i > 0) {
          const float prev = post[size_t(i - 1) * 2 + size_t(ch)];
          max_deriv = std::max(max_deriv, std::fabs(s - prev));
        }
      }
    }
    std::printf("hitch recovery (offline 2s): non_finite=%d clipped=%d max_abs=%.4f max_deriv=%.4f rms=%.4f\n",
                non_finite, clipped, max_abs, max_deriv, std::sqrt(energy / double(kPostFrames * 2)));
    CHECK(non_finite == 0);
    CHECK(clipped == 0);
    CHECK(max_abs <= 1.0001f);
    CHECK(max_deriv < 0.45f);
    CHECK(std::sqrt(energy / double(kPostFrames * 2)) > 0.01);
  }

  // Part B: Real-time audio thread with a simulated 250 ms stream underrun hitch
  // verifying smooth zero-click fade-in recovery, bounded catch-up, and clean output.
  {
    audio::AudioEngine eng(kRate);
    auto ep = std::make_unique<audio::EnginePlayer>(
        sound, std::vector<std::vector<const audio::PcmSample*>>{{&loop_pcm, &loop_pcm, &loop_pcm}}, kRate, 900.0,
        8000.0);
    ep->master = 0.7;
    eng.set_engine_player(std::move(ep));
    eng.set_engine_input({4200.0, 0.7, 0.6, 0.0, audio::View::kFollow});

    std::atomic<int> virtual_queued{0};
    std::atomic<bool> inject_underrun{false};
    std::atomic<bool> saw_underrun_recovery{false};
    std::atomic<float> recovery_first_sample{99.0f};
    std::atomic<int> bad_samples{0};
    std::atomic<int> clip_samples{0};
    std::atomic<int> total_blocks{0};

    audio::AudioEngine::Sink sink;
    sink.target_buffer_frames = int(0.08 * kRate);  // 80 ms
    sink.block_frames = kBlock;
    sink.queued_frames = [&]() -> int {
      if (inject_underrun.exchange(false, std::memory_order_acq_rel)) {
        virtual_queued.store(0, std::memory_order_release);
        saw_underrun_recovery.store(true, std::memory_order_release);
        return 0;
      }
      return virtual_queued.load(std::memory_order_acquire);
    };
    sink.write = [&](const float* stereo, int frames) {
      if (saw_underrun_recovery.exchange(false, std::memory_order_acq_rel) && frames > 0) {
        recovery_first_sample.store(std::max(std::fabs(stereo[0]), std::fabs(stereo[1])), std::memory_order_release);
      }
      for (int i = 0; i < frames * 2; ++i) {
        if (!std::isfinite(stereo[i])) bad_samples.fetch_add(1, std::memory_order_relaxed);
        else if (std::fabs(stereo[i]) > 1.0001f) clip_samples.fetch_add(1, std::memory_order_relaxed);
      }
      virtual_queued.fetch_add(frames, std::memory_order_release);
      total_blocks.fetch_add(1, std::memory_order_relaxed);
    };

    eng.start(std::move(sink));
    const auto t_fill = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (total_blocks.load(std::memory_order_acquire) < 8 && std::chrono::steady_clock::now() < t_fill) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const int before_underrun = total_blocks.load(std::memory_order_acquire);
    // Simulate a 250 ms hitch draining the hardware buffer completely to 0.
    inject_underrun.store(true, std::memory_order_release);
    const auto t_rec = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (total_blocks.load(std::memory_order_acquire) < before_underrun + 4 &&
           std::chrono::steady_clock::now() < t_rec) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto tel = eng.telemetry();
    eng.stop();

    std::printf("hitch recovery (rt underrun): blocks=%d underruns=%llu first_rec=%.6f bad=%d clip=%d\n",
                total_blocks.load(), (unsigned long long)tel.underruns, recovery_first_sample.load(),
                bad_samples.load(), clip_samples.load());
    CHECK(bad_samples.load() == 0);
    CHECK(clip_samples.load() == 0);
    CHECK(tel.underruns >= 1);
    // Smooth half-cosine fade-in starts at exact 0.0 on the first recovered sample (zero pop).
    CHECK(recovery_first_sample.load() < 1e-4f);
  }
}

void phase2_parity_checks() {
  // 1. SatNavVoPlayer FIFO queue plays back-to-back cues sequentially & interrupts on kRecalculating.
  {
    audio::SatNavVoPlayer sn(48000);
    audio::PcmSample a, b, c;
    a.rate = b.rate = c.rate = 48000;
    a.data.assign(480, 0.25f);
    b.data.assign(480, 0.50f);
    c.data.assign(480, 0.80f);
    sn.set_clip(audio::SatNavCue::kDestGeneric, a);
    sn.set_clip(audio::SatNavCue::kTurnLeft, b);
    sn.set_clip(audio::SatNavCue::kRecalculating, c);
    sn.trigger(audio::SatNavCue::kDestGeneric);
    sn.trigger(audio::SatNavCue::kTurnLeft);
    std::vector<float> buf(480 * 2, 0.0f);
    CHECK(sn.render(buf.data(), 480));
    CHECK(std::fabs(buf[10] - 0.25f) < 0.05f);
    std::fill(buf.begin(), buf.end(), 0.0f);
    CHECK(sn.render(buf.data(), 480));
    CHECK(std::fabs(buf[10] - 0.50f) < 0.05f);
    // Interrupting cue immediately takes over.
    sn.trigger(audio::SatNavCue::kDestGeneric);
    sn.trigger(audio::SatNavCue::kTurnLeft);
    sn.trigger(audio::SatNavCue::kRecalculating);
    std::fill(buf.begin(), buf.end(), 0.0f);
    CHECK(sn.render(buf.data(), 480));
    CHECK(std::fabs(buf[10] - 0.80f) < 0.05f);
  }

  // 2. Tires.xml surface-group mapping.
  {
    audio::SfxEngine sfx(48000);
    CHECK(sfx.map_surface_group("Asphalt") == 0);
    CHECK(sfx.map_surface_group("Gravel") == 1);
    CHECK(sfx.map_surface_group("Grass") == 2);
    CHECK(sfx.map_surface_group("Cobblestone") == 3);
    CHECK(sfx.map_surface_group("WoodBridge") == 4);
  }

  // 3. New UiCue stings render onto Bus::kUi.
  {
    audio::SfxEngine sfx(48000);
    audio::PcmSample tone;
    tone.rate = 48000;
    tone.data.assign(480, 0.5f);
    sfx.set_sample(audio::SfxSoundId::kUiCountdownGo, tone);
    sfx.set_sample(audio::SfxSoundId::kUiSpeedCamera, tone);
    sfx.set_sample(audio::SfxSoundId::kUiPhotoShutter, tone);
    sfx.set_sample(audio::SfxSoundId::kUiPurchase, tone);
    sfx.set_sample(audio::SfxSoundId::kUiCarAdded, tone);
    sfx.set_sample(audio::SfxSoundId::kUiH2hGo, tone);
    sfx.set_sample(audio::SfxSoundId::kUiH2hLoss, tone);
    audio::VehicleAudioState vs{};
    std::array<std::vector<float>, audio::kBuses> bufs;
    float* ptrs[audio::kBuses]{};
    for (int i = 0; i < audio::kBuses; ++i) {
      bufs[size_t(i)].assign(480 * 2, 0.0f);
      ptrs[i] = bufs[size_t(i)].data();
    }
    for (audio::UiCue cue : {audio::UiCue::kCountdownGo, audio::UiCue::kSpeedCamera, audio::UiCue::kPhotoShutter,
                             audio::UiCue::kPurchase, audio::UiCue::kCarAdded, audio::UiCue::kH2hGo,
                             audio::UiCue::kH2hLoss}) {
      for (auto& b : bufs) std::fill(b.begin(), b.end(), 0.0f);
      sfx.trigger_ui(cue);
      sfx.render(vs, ptrs, 480);
      double energy = 0.0;
      for (float v : bufs[int(audio::Bus::kUi)]) energy += double(v) * double(v);
      CHECK(energy > 0.1);
    }
  }

  // 4. AudioEngine playback_system (TV vs Home Theater) is independent of full_dynamic_range.
  {
    audio::AudioEngine eng(48000);
    eng.set_full_dynamic_range(true);
    eng.set_playback_system(false);
    eng.set_ui_music(audio::AudioEngine::UiMusicMode::kTitle);
    std::vector<float> pcm(480 * 2, 0.0f);
    eng.render_offline(pcm.data(), 480);
    for (float v : pcm) CHECK(std::isfinite(v));
  }
}

void phase_r2_1_audio_parity_checks() {
  auto make_tone = [](double hz, int frames, int rate = 48000) {
    audio::PcmSample s;
    s.rate = rate;
    s.data.resize(size_t(frames));
    for (int i = 0; i < frames; ++i)
      s.data[size_t(i)] = float(0.6 * std::sin(2.0 * 3.14159265358979323846 * hz * double(i) / double(rate)));
    return s;
  };
  auto rms = [](const std::vector<float>& buf) {
    double acc = 0.0;
    for (float v : buf) acc += double(v) * double(v);
    return buf.empty() ? 0.0 : std::sqrt(acc / double(buf.size()));
  };

  // 1. P0-A2: Lock-free SpscRingBuffer wrap-around & concurrent producer/consumer FIFO integrity.
  {
    audio::SpscRingBuffer<float> rb(100);
    CHECK(rb.capacity() == 128);  // power-of-two rounding
    std::vector<float> in(90), out(90, 0.0f);
    for (size_t i = 0; i < in.size(); ++i) in[i] = float(i + 1);
    CHECK(rb.write(in.data(), 90) == 90);
    CHECK(rb.available_read() == 90 && rb.available_write() == 38);
    CHECK(rb.read(out.data(), 60) == 60);
    CHECK(out[0] == 1.0f && out[59] == 60.0f);
    // Write 80 across wrap boundary.
    std::vector<float> in2(80);
    for (size_t i = 0; i < 80; ++i) in2[i] = float(100 + i);
    CHECK(rb.write(in2.data(), 80) == 80);
    std::vector<float> drain(110, 0.0f);
    CHECK(rb.read(drain.data(), 110) == 110);
    CHECK(drain[0] == 61.0f && drain[29] == 90.0f && drain[30] == 100.0f && drain[109] == 179.0f);
    CHECK(rb.available_read() == 0);

    // Concurrent SPSC stress (100,000 floats).
    audio::SpscRingBuffer<float> srb(4096);
    constexpr size_t kTotal = 100000;
    std::atomic<bool> ok{true};
    std::thread prod([&]() {
      size_t sent = 0;
      float chunk[257];
      while (sent < kTotal) {
        const size_t n = std::min<size_t>(257, kTotal - sent);
        for (size_t i = 0; i < n; ++i) chunk[i] = float(sent + i);
        size_t off = 0;
        while (off < n) off += srb.write(chunk + off, n - off);
        sent += n;
      }
    });
    std::thread cons([&]() {
      size_t got = 0;
      float chunk[193];
      while (got < kTotal) {
        const size_t n = srb.read(chunk, std::min<size_t>(193, kTotal - got));
        for (size_t i = 0; i < n; ++i) {
          if (chunk[i] != float(got + i)) ok.store(false, std::memory_order_relaxed);
        }
        got += n;
      }
    });
    prod.join();
    cons.join();
    CHECK(ok.load());
  }

  // 2. P0-A8: Multichannel FSB4 xma2_wave stream count ((channels + 1) / 2) and channel masks.
  {
    std::vector<uint8_t> pkt(2048, 0);
    auto u16_at = [](const std::vector<uint8_t>& b, size_t off) {
      return uint16_t(b[off] | (uint16_t(b[off + 1]) << 8));
    };
    auto u32_at = [](const std::vector<uint8_t>& b, size_t off) {
      return uint32_t(b[off]) | (uint32_t(b[off + 1]) << 8) | (uint32_t(b[off + 2]) << 16) |
             (uint32_t(b[off + 3]) << 24);
    };
    struct Case {
      uint16_t ch;
      uint16_t want_streams;
      uint32_t want_mask;
    };
    for (const Case c : std::initializer_list<Case>{
             {1, 1, 0x4u}, {2, 1, 0x3u}, {4, 2, 0x33u}, {6, 3, 0x3Fu}, {8, 4, 0x63Fu}}) {
      audio::FsbSample s;
      s.channels = c.ch;
      s.rate = 48000;
      s.frames = 1024;
      const auto w = audio::xma2_wave(s, pkt);
      // RIFF(12) + fmt_hdr(8) + WAVEFORMATEX(18) = offset 38 -> num_streams(u16), offset 40 -> mask(u32).
      CHECK(w.size() >= 72);
      CHECK(u16_at(w, 22) == c.ch);
      CHECK(u16_at(w, 38) == c.want_streams);
      CHECK(u32_at(w, 40) == c.want_mask);
    }
  }

  // 3. P0-A4: AudioEngine::trigger_collision propagates surface_hint & SmashMaterial all the way to SfxEngine.
  {
    audio::AudioEngine eng(48000);
    auto sfx = std::make_unique<audio::SfxEngine>(48000);
    // Populate ONLY kImpactMetal (leave kImpact3d empty!) so Metal surface_hint=3 MUST propagate to produce audio.
    sfx->set_sample(audio::SfxSoundId::kImpactMetal, make_tone(440.0, 2400));
    sfx->set_sample(audio::SfxSoundId::kBreakFence, make_tone(620.0, 2400));
    sfx->set_sample(audio::SfxSoundId::kBreakGlass, make_tone(920.0, 2400));
    auto* sfx_raw = sfx.get();
    eng.set_sfx_engine(std::move(sfx));

    audio::CollisionHit metal_hit{};
    metal_hit.rel_speed_mps = 15.0f;
    metal_hit.pos = {1.0f, 0.0f, 0.0f};
    metal_hit.surface_hint = 3;  // Metal/Guardrail
    eng.trigger_collision(metal_hit);
    std::vector<float> out(4800 * 2, 0.0f);
    eng.render_offline(out.data(), 4800);
    CHECK(rms(out) > 0.02);
    CHECK(sfx_raw->last_one_shot_id() == audio::SfxSoundId::kImpactMetal);

    // Wait past 75 ms collision cooldown (4800 frames = 100 ms already rendered).
    audio::CollisionHit fence_break{};
    fence_break.rel_speed_mps = 14.0f;
    fence_break.pos = {0.0f, 0.0f, 1.0f};
    fence_break.is_break = true;
    fence_break.smash_material = audio::SmashMaterial::kFence;
    eng.trigger_collision(fence_break);
    std::fill(out.begin(), out.end(), 0.0f);
    eng.render_offline(out.data(), 4800);
    CHECK(rms(out) > 0.02);
    CHECK(sfx_raw->last_one_shot_id() == audio::SfxSoundId::kBreakFence);

    audio::CollisionHit glass_break{};
    glass_break.rel_speed_mps = 14.0f;
    glass_break.pos = {0.0f, 0.0f, 1.0f};
    glass_break.is_break = true;
    glass_break.smash_material = audio::SmashMaterial::kGlass;
    eng.trigger_collision(glass_break);
    std::fill(out.begin(), out.end(), 0.0f);
    eng.render_offline(out.data(), 4800);
    CHECK(rms(out) > 0.02);
    CHECK(sfx_raw->last_one_shot_id() == audio::SfxSoundId::kBreakGlass);
  }

  // 4. P0-A6: Turbo/SC tier indexing (0=none, 1=L1, 2=L2, 3=L3) & per-car blowoffChoice selection.
  {
    constexpr int kFrames = 2400;
    std::array<std::vector<float>, audio::kBuses> bufs;
    float* ptrs[audio::kBuses]{};
    auto clear_bufs = [&]() {
      for (int b = 0; b < audio::kBuses; ++b) {
        bufs[size_t(b)].assign(kFrames * 2, 0.0f);
        ptrs[b] = bufs[size_t(b)].data();
      }
    };

    // 4a. turbo_level == 0 -> silent; turbo_level == 1 -> L1; 2 -> L2; 3 -> L3.
    {
      audio::SfxEngine sfx(48000);
      sfx.set_sample(audio::SfxSoundId::kTurboSpool, make_tone(1000.0, 9600));
      sfx.set_sample(audio::SfxSoundId::kTurboSpoolL2, make_tone(1400.0, 9600));
      sfx.set_sample(audio::SfxSoundId::kScWhineL3, make_tone(1200.0, 9600));

      audio::VehicleAudioState st{};
      st.forced_induction = 1;
      st.turbo_level = 0;  // 0 = none!
      st.turbo_norm_rpm = 0.9f;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kTurbo)]) < 1e-6);

      // turbo_level == 2 must pick L2 (not L3); verify by setting ONLY L2 in a fresh SfxEngine.
      audio::SfxEngine sfx_l2(48000);
      sfx_l2.set_sample(audio::SfxSoundId::kTurboSpoolL2, make_tone(1400.0, 9600));
      st.turbo_level = 2;
      clear_bufs();
      sfx_l2.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kTurbo)]) > 0.02);

      // turbo_level == 1 must NOT play L2 (only L1, which is empty in sfx_l2).
      audio::SfxEngine sfx_l2_only(48000);
      sfx_l2_only.set_sample(audio::SfxSoundId::kTurboSpoolL2, make_tone(1400.0, 9600));
      st.turbo_level = 1;
      clear_bufs();
      sfx_l2_only.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kTurbo)]) < 1e-6);

      // Supercharger: sc_level == 0 -> silent; sc_level == 3 -> kScWhineL3.
      st.forced_induction = 2;
      st.sc_level = 0;
      st.supercharger_norm_rpm = 0.9f;
      st.throttle = 1.0f;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kSupercharger)]) < 1e-6);
      st.sc_level = 3;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kSupercharger)]) > 0.02);
    }

    // 4b. Per-car blowoffL2Choice == 4 selects kTurboBovL2C4Hi (not hardcoded Choice 1).
    {
      audio::SfxEngine sfx(48000);
      const auto want_id = audio::turbo_bov_sound_id(2, 4, true);
      CHECK(want_id == audio::SfxSoundId::kTurboBovL2C4Hi);
      sfx.set_sample(want_id, make_tone(820.0, 4800));
      audio::VehicleAudioState st{};
      st.forced_induction = 1;
      st.turbo_level = 2;
      st.turbo_psi = 16.0f;  // > 13.0 -> Hi
      st.bov_open = true;
      st.blowoff_level = {1, 2, 3};
      st.blowoff_choice = {1, 4, 2};
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kTurbo)]) > 0.02);
      CHECK(sfx.last_one_shot_id() == want_id);
    }
  }

  // 5. P0-A7: DLC LOD1_pri_1650 discovery, TU4 XML layering, Rally subsystems & SetRadioRallyMode.
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "shift_audio_r2_1_test";
    fs::remove_all(tmp, ec);

    // 5a. LOD1_pri_1650 discovery in SoundFiles::open.
    const fs::path media = tmp / "disc" / "media";
    const fs::path pkg_dz = tmp / "state" / "user" / "0000000000000000" / "4D5309C9" / "00000002" / "PKG_RALLY" /
                            "Media" / "DLCZips";
    const fs::path lod1_pri = pkg_dz / "LOD1_pri_1650";
    fs::create_directories(media / "audio" / "cars" / "Engines" / "Soundbanks" / "LOD1", ec);
    fs::create_directories(lod1_pri, ec);
    { std::ofstream(lod1_pri / "DLC_RallyCar_Exh.fsb") << "stub"; }
    cars::Install inst;
    inst.media = media;
    cars::Package pkg;
    pkg.id = "PKG_RALLY";
    pkg.archives.push_back(pkg_dz / "1600_pri_65");  // sibling dir in same DLCZips parent
    fs::create_directories(pkg_dz / "1600_pri_65", ec);
    inst.packages.push_back(std::move(pkg));
    audio::SoundFiles sf;
    std::string err;
    CHECK(sf.open(inst, &err));
    CHECK(!sf.bank("DLC_RallyCar_Exh.fsb").empty());

    // 5b. TU4 overrides for AudioEngineConfig.xml, SurfaceChanges.xml, CollisionData.xml.
    const fs::path game_root = tmp / "disc";
    const fs::path state_root = tmp / "state";
    fs::create_directories(game_root / "media" / "audio" / "cars" / "Tires", ec);
    fs::create_directories(game_root / "media" / "audio" / "collisions", ec);
    fs::create_directories(state_root / "title-update-v4" / "media" / "audio" / "cars" / "Tires", ec);
    fs::create_directories(state_root / "title-update-v4" / "media" / "audio" / "collisions", ec);
    {
      std::ofstream(game_root / "media" / "audio" / "AudioEngineConfig.xml")
          << R"(<AudioEngineConfig compressorRatio="0.25" saturatorSaturation="0.90"/>)";
      std::ofstream(state_root / "title-update-v4" / "media" / "audio" / "AudioEngineConfig.xml")
          << R"(<AudioEngineConfig compressorRatio="0.33"/>)";
      std::ofstream(game_root / "media" / "audio" / "cars" / "Tires" / "Tires.xml")
          << R"(<Tires><SurfaceMap PhysicsSurface="Asphalt" Lod1EventGroup="OnRoad"/></Tires>)";
      std::ofstream(game_root / "media" / "audio" / "cars" / "Tires" / "SurfaceChanges.xml")
          << R"(<SurfaceChanges><Surfaces Surface1="Asphalt" Surface2="Dirt"><S1toS2 Event="HardToDirt"/></Surfaces></SurfaceChanges>)";
      std::ofstream(state_root / "title-update-v4" / "media" / "audio" / "cars" / "Tires" / "SurfaceChanges.xml")
          << R"(<SurfaceChanges><Surfaces Surface1="Asphalt" Surface2="RallyGravel"><S1toS2 Event="HardToRallyGravel"/></Surfaces></SurfaceChanges>)";
      std::ofstream(game_root / "media" / "audio" / "collisions" / "CollisionData.xml")
          << R"(<CollisionData MSBetweenDiffCollisions="75"><CollisionEvent Surface1="Asphalt" Surface2="CarAluminum"><Impact3D Group="G" Event="E1"/></CollisionEvent></CollisionData>)";
      std::ofstream(state_root / "title-update-v4" / "media" / "audio" / "collisions" / "CollisionData.xml")
          << R"(<CollisionData MSBetweenDiffCollisions="60"><CollisionEvent Surface1="RallyGravel" Surface2="CarAluminum"><Impact3D Group="G" Event="E2"/></CollisionEvent></CollisionData>)";
    }
    audio::MasterDspConfig mdsp;
    CHECK(mdsp.load(game_root, nullptr, state_root));
    CHECK(near(mdsp.compressor_ratio, 0.33, 1e-4) && near(mdsp.saturator_saturation, 0.90, 1e-4));
    audio::TiresConfig tcfg;
    CHECK(tcfg.load(game_root, nullptr, state_root));
    CHECK(tcfg.surface_change_event("Asphalt", "Dirt") == "HardToDirt");
    CHECK(tcfg.surface_change_event("Asphalt", "RallyGravel") == "HardToRallyGravel");
    audio::CollisionAudioConfig ccfg;
    CHECK(ccfg.load(game_root, nullptr, state_root));
    CHECK(near(ccfg.ms_between_diff_collisions, 60.0, 1e-3));
    CHECK(ccfg.find_pair("Asphalt", "CarAluminum") != nullptr);
    CHECK(ccfg.find_pair("RallyGravel", "CarAluminum") != nullptr);

    // 5c. SetRadioRallyMode attenuates GameMusic while keeping RallyCoDriver at 1.0.
    audio::SnapshotMatrix mat;
    const std::string snap_xml = R"(
      <AudioMixerSnapshots>
        <FreeRoam group="Gameplay">
          <GameMusic active="1" destvolume="1.0"/>
          <RallyCoDriver active="1" destvolume="1.0"/>
        </FreeRoam>
      </AudioMixerSnapshots>
    )";
    CHECK(mat.parse(snap_xml));
    CHECK(near(mat.gain(audio::Category::kGameMusic), 1.0, 1e-4));
    mat.set_radio_rally_mode(true, 0.0f);
    CHECK(mat.radio_rally_mode());
    CHECK(mat.gain(audio::Category::kGameMusic) < 0.35f);
    CHECK(near(mat.gain(audio::Category::kRallyCoDriver), 1.0, 1e-4));
    mat.set_radio_rally_mode(false, 0.0f);
    CHECK(near(mat.gain(audio::Category::kGameMusic), 1.0, 1e-4));

    // 5d. AudioEngine::SetRadioRallyMode + trigger_landing + 13-region ColoradoDirt ambience + Rally_Tires.
    audio::AudioEngine eng(48000);
    auto sfx = std::make_unique<audio::SfxEngine>(48000);
    sfx->set_sample(audio::SfxSoundId::kCarLandingHeavy, make_tone(85.0, 2400));
    sfx->set_sample(audio::SfxSoundId::kRallyTireRollGravel, make_tone(310.0, 4800));
    auto* sfx_ptr = sfx.get();
    eng.set_sfx_engine(std::move(sfx));
    eng.SetRadioRallyMode(true);
    CHECK(eng.radio_rally_mode());

    audio::LandingHit lh{};
    lh.impact_speed_mps = 7.0f;
    lh.air_time_s = 0.85f;
    lh.surface_hint = 1;
    eng.trigger_landing(lh);
    std::vector<float> lout(2400 * 2, 0.0f);
    eng.render_offline(lout.data(), 2400);
    CHECK(rms(lout) > 0.02);
    CHECK(sfx_ptr->last_one_shot_id() == audio::SfxSoundId::kCarLandingHeavy);
    CHECK(eng.telemetry().radio_rally_mode);

    // Rally_Tires: rally_mode=true plays kRallyTireRollGravel on OffRoad/Gravel wheel surface.
    audio::VehicleAudioState rst{};
    rst.speed_mph = 60.0f;
    rst.rally_mode = true;
    for (auto& w : rst.wheels) {
      w.surface_group = 1;
      w.load = 1.0f;
    }
    eng.set_vehicle_state(rst);
    std::vector<float> rout(2400 * 2, 0.0f);
    eng.render_offline(rout.data(), 2400);
    CHECK(rms(rout) > 0.005);

    // 13-region ColoradoDirt ambience selection.
    audio::AmbienceBedPlayer amb(48000);
    amb.set_rally_world(true);
    for (int r = 0; r < audio::AmbienceBedPlayer::kMaxRallyRegions; ++r) {
      amb.set_rally_bed_sample(r, "region" + std::to_string(r), make_tone(120.0 + 15.0 * r, 4800));
    }
    audio::AcousticsSample ac{};
    ac.is_rally_world = true;
    ac.ambience_snapshot = "region9";
    std::vector<float> aout(2400 * 2, 0.0f);
    amb.render(ac, 15.0f, aout.data(), 2400);
    CHECK(amb.rally_bed_count() == 13);
    CHECK(amb.last_rally_region() == 9);
    CHECK(rms(aout) > 0.005);

    fs::remove_all(tmp, ec);
  }

  // Phase R2-4: Sat-Nav VO cue fallback uniqueness (kNearBearRight != kNearRight) & new retail cues.
  {
    std::vector<std::string> needles;
    for (int i = 0; i < int(audio::SatNavCue::kCount); ++i) {
      const std::string n = audio::satnav_cue_fsb_fallback(audio::SatNavCue(i));
      CHECK(!n.empty());
      for (const auto& prev : needles) CHECK(prev != n);
      needles.push_back(n);
    }
    CHECK(std::string(audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearBearRight)) !=
          std::string(audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearRight)));
    audio::SatNavVoPlayer sn(48000);
    audio::PcmSample clip;
    clip.rate = 48000;
    clip.data.assign(2400, 0.4f);
    sn.set_clip(audio::SatNavCue::kExtendedLeft, clip);
    sn.set_clip(audio::SatNavCue::kNextJuncRight, clip);
    sn.set_clip(audio::SatNavCue::kFarDestination, clip);
    sn.set_clip(audio::SatNavCue::kDestCannotBeSet, clip);
    sn.trigger(audio::SatNavCue::kExtendedLeft);
    sn.trigger(audio::SatNavCue::kDestCannotBeSet);  // high-priority interrupt
    std::vector<float> buf(1200 * 2, 0.0f);
    CHECK(sn.render(buf.data(), 1200));
  }
}

void phase_r2_5_audio_completeness_checks() {
  auto make_tone = [](double hz, int frames, int rate = 48000, float amp = 0.6f) {
    audio::PcmSample s;
    s.rate = rate;
    s.data.resize(size_t(frames));
    for (int i = 0; i < frames; ++i)
      s.data[size_t(i)] = float(double(amp) * std::sin(2.0 * 3.14159265358979323846 * hz * double(i) / double(rate)));
    return s;
  };
  auto rms = [](const std::vector<float>& buf) {
    double acc = 0.0;
    for (float v : buf) acc += double(v) * double(v);
    return buf.empty() ? 0.0 : std::sqrt(acc / double(buf.size()));
  };

  // 1. HomespaceMusic, MenuMusic, and HomespaceAmbience are isolated from GameMusic (-13 dB) & AmbientStatic (-6 dB).
  {
    const std::string xml = R"(
      <AudioMixerSnapshots>
        <UIExterior group="Gameplay" fadein="0.0" fadeout="0.0">
          <GameMusic active="1" destvolume="0.2239"/>
          <HomespaceMusic active="1" destvolume="1.0"/>
          <MenuMusic active="1" destvolume="1.0"/>
          <AmbientStatic active="1" destvolume="0.5012"/>
          <HomespaceAmbientDynamic active="1" destvolume="1.0"/>
        </UIExterior>
      </AudioMixerSnapshots>
    )";
    audio::SnapshotMatrix mat;
    CHECK(mat.parse(xml));
    mat.activate("UIExterior");
    CHECK(near(mat.bus_gain(audio::Bus::kMusic), 0.2239, 1e-3));
    CHECK(near(mat.bus_gain(audio::Bus::kHomespaceMusic), 1.0, 1e-3));
    CHECK(near(mat.bus_gain(audio::Bus::kMenuMusic), 1.0, 1e-3));
    CHECK(near(mat.bus_gain(audio::Bus::kAmbience), 0.5012, 1e-3));
    CHECK(near(mat.bus_gain(audio::Bus::kHomespaceAmbience), 1.0, 1e-3));
  }

  // 2. Top-level UI bus sliders propagate cleanly to all child sub-buses.
  {
    audio::Mixer m;
    m.prepare(48000);
    m.set_bus_level(audio::Bus::kEngine, 0.40f);
    CHECK(near(m.bus_level(audio::Bus::kTransmission), 0.40, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kSupercharger), 0.40, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kTurbo), 0.40, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kBackfire), 0.40, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kEngineDamage), 0.40, 1e-4));
    m.set_bus_level(audio::Bus::kVo, 0.55f);
    CHECK(near(m.bus_level(audio::Bus::kSatNav), 0.55, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kRallyCoDriver), 0.55, 1e-4));
    m.set_bus_level(audio::Bus::kMusic, 0.35f);
    CHECK(near(m.bus_level(audio::Bus::kHomespaceMusic), 0.35, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kMenuMusic), 0.35, 1e-4));
    m.set_bus_level(audio::Bus::kAmbience, 0.50f);
    CHECK(near(m.bus_level(audio::Bus::kHomespaceAmbience), 0.50, 1e-4));
    CHECK(near(m.bus_level(audio::Bus::kCollision), 0.50, 1e-4));
    m.set_bus_level(audio::Bus::kTyres, 0.65f);
    CHECK(near(m.bus_level(audio::Bus::kSuspension), 0.65, 1e-4));

    audio::AudioEngine eng(48000);
    eng.set_bus_level(audio::Bus::kEngine, 0.45f);
    eng.set_bus_level(audio::Bus::kVo, 0.60f);
    CHECK(near(eng.bus_level(audio::Bus::kTurbo), 0.45, 1e-4));
    CHECK(near(eng.bus_level(audio::Bus::kEngineDamage), 0.45, 1e-4));
    CHECK(near(eng.bus_level(audio::Bus::kSatNav), 0.60, 1e-4));
    CHECK(near(eng.bus_level(audio::Bus::kRallyCoDriver), 0.60, 1e-4));
  }

  // 3. PlaybackSystem curves (HomeTheater, TV, Headphones, CarAudio, FullDynamicRange) wired cleanly.
  {
    audio::MasterRmsStage st;
    st.prepare(48000);
    st.set_playback_system(audio::PlaybackSystem::kHomeTheater);
    CHECK(st.playback_system() == audio::PlaybackSystem::kHomeTheater);
    st.set_playback_system(audio::PlaybackSystem::kTv);
    CHECK(st.playback_system() == audio::PlaybackSystem::kTv);
    st.set_playback_system(audio::PlaybackSystem::kHeadphones);
    CHECK(st.playback_system() == audio::PlaybackSystem::kHeadphones);
    st.set_playback_system(audio::PlaybackSystem::kCarAudio);
    CHECK(st.playback_system() == audio::PlaybackSystem::kCarAudio);
    st.set_playback_system(audio::PlaybackSystem::kFullDynamicRange);
    CHECK(st.playback_system() == audio::PlaybackSystem::kFullDynamicRange && st.full_dynamic_range());

    audio::AudioEngine eng(48000);
    eng.set_playback_system(audio::PlaybackSystem::kHeadphones);
    std::vector<float> pcm(480 * 2, 0.0f);
    eng.render_offline(pcm.data(), 480);
    for (float v : pcm) CHECK(std::isfinite(v));
  }

  // 4. Cockpit _CMT.xml <OtherVolumes> & Focus _ET.xml DSP (drivetrain_spring, FocusPEQ, Distortion, TrashDSP).
  {
    const std::string et_xml = R"(
      <Engine>
        <EngineSettings audio_rpm_idle="900" audio_rpm_redline="7500"
                        drivetrain_spring_enabled="1" drivetrain_springF1="14.0" drivetrain_damperR1="3.5"
                        drivetrain_springF2="28.0" drivetrain_damperR2="6.0" drivetrain_scalar="2.0"
                        drivetrain_clamp="400.0"/>
        <VolumeAdjustments BurbleVolume="0.75"/>
        <FocusPEQ Active="1" CenterFrequency="1200" Bandwidth="1.2" Gain="1.6"/>
        <Distortion Active="1" Level="0.5" VolumeCompensate="0.85"/>
        <TrashDSP Active="1" Cutoff1="350" Cutoff2="2400">
          <Low inputgain="1.2" overdrive="0.4" mix="0.5" outputgain="1.0"/>
          <Mid inputgain="1.1" overdrive="0.6" mix="0.4" outputgain="1.0"/>
          <High inputgain="1.0" overdrive="0.3" mix="0.25" outputgain="1.0"/>
        </TrashDSP>
      </Engine>
    )";
    const std::string cmt_xml = R"(
      <CarModelTuning>
        <Listener>
          <Basic>
            <Cockpit>
              <OtherVolumes Wind="0.35" Tires="0.40" Transmission="1.25" Turbo="0.80" Backfire="0.55" EngineDamage="1.15"/>
            </Cockpit>
            <Follow>
              <OtherVolumes Wind="1.0" Tires="1.0" Transmission="1.0" Turbo="1.0" Backfire="1.0" EngineDamage="1.0"/>
            </Follow>
          </Basic>
        </Listener>
      </CarModelTuning>
    )";
    audio::XmlNode et_node, cmt_node;
    std::string err;
    CHECK(audio::parse_xml(et_xml, et_node, &err));
    CHECK(audio::parse_xml(cmt_xml, cmt_node, &err));
    audio::EngineSound es;
    std::vector<audio::HarmonicRef> refs;
    CHECK(audio::read_engine_tuning(et_node, es, refs, &err));
    audio::read_model_tuning(cmt_node, es);
    CHECK(es.drivetrain_spring.enabled && near(es.drivetrain_spring.spring_f1, 14.0, 1e-3));
    CHECK(es.focus_peq.active && near(es.focus_peq.center_freq.eval({0.5, 0.5, 0.5, 0.0}), 1200.0, 1e-3));
    CHECK(es.distortion.active);
    CHECK(es.trash_dsp.active && near(es.trash_dsp.cutoff1_hz, 350.0, 1e-3));
    CHECK(es.has_other_volumes);
    CHECK(near(es.other_volumes[size_t(audio::View::kCockpit)].wind, 0.35, 1e-4));
    CHECK(near(es.other_volumes[size_t(audio::View::kCockpit)].tires, 0.40, 1e-4));
    CHECK(near(es.other_volumes[size_t(audio::View::kCockpit)].transmission, 1.25, 1e-4));
    CHECK(near(es.burble_volume, 0.75, 1e-4));

    // Add a synthetic loop group and verify EnginePlayer runs FocusPEQ/Distortion/TrashDSP + drivetrain spring cleanly.
    audio::Group g;
    g.name = "ExhaustL";
    g.bank = "synth.fsb";
    audio::Loop lp = loop(800, 3000, 8000);
    lp.sample = 0;
    g.loops.push_back(lp);
    es.groups.push_back(g);
    const audio::PcmSample tone = make_tone(180.0, 4800, 48000, 0.5f);
    audio::EnginePlayer ep(es, {{&tone}}, 48000, 900.0, 7500.0);
    ep.set({3000.0, 0.0, 0.0, 0.0, audio::View::kFollow});
    std::vector<float> buf(960 * 2, 0.0f);
    ep.render(buf.data(), 960);
    // Sudden full-throttle torque step excites drivetrain spring oscillation!
    ep.set({3400.0, 1.0, 1.0, 0.0, audio::View::kFollow});
    ep.render(buf.data(), 960);
    CHECK(rms(buf) > 0.02);
    CHECK(std::fabs(ep.drivetrain_spring_offset()) > 0.1 || ep.distortion_level() > 0.1);
    for (float v : buf) CHECK(std::isfinite(v));
  }

  // 5. Bus::kEngineDamage output (procedural fallback & bank sample), Burble vs Backfire, ExhNoise, Multi-band EV.
  {
    constexpr int kFrames = 2400;
    std::array<std::vector<float>, audio::kBuses> bufs;
    float* ptrs[audio::kBuses]{};
    auto clear_bufs = [&]() {
      for (int b = 0; b < audio::kBuses; ++b) {
        bufs[size_t(b)].assign(kFrames * 2, 0.0f);
        ptrs[b] = bufs[size_t(b)].data();
      }
    };

    // 5a. Procedural fallback on Bus::kEngineDamage produces audible output even without Damage.fev loaded!
    {
      audio::SfxEngine sfx(48000);
      audio::VehicleAudioState st{};
      st.rpm = 3800.0f;
      st.idle_rpm = 800.0f;
      st.redline_rpm = 7500.0f;
      st.engine_damage = 0.0f;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kEngineDamage)]) < 1e-6);

      st.engine_damage = 0.75f;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kEngineDamage)]) > 0.01);
    }

    // 5b. Distinct Burble overrun crackle vs shift Backfire pop, ExhNoise, and Multi-band EV.
    {
      audio::SfxEngine sfx(48000);
      sfx.set_sample(audio::SfxSoundId::kBurblePop0, make_tone(260.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kBackfirePop, make_tone(120.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kExhNoise, make_tone(780.0, 4800));
      sfx.set_sample(audio::SfxSoundId::kElectricMotor, make_tone(320.0, 4800));
      sfx.set_sample(audio::SfxSoundId::kElectricMotorMid, make_tone(640.0, 4800));
      sfx.set_sample(audio::SfxSoundId::kElectricMotorHigh, make_tone(1280.0, 4800));

      audio::VehicleAudioState st{};
      st.rpm = 5600.0f;
      st.idle_rpm = 800.0f;
      st.redline_rpm = 7500.0f;
      st.throttle = 0.85f;
      st.backfire_level = 2;
      st.exh_noise_vol_scale = 1.0f;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      // High-RPM ExhNoise renders onto Bus::kEngine.
      CHECK(rms(bufs[int(audio::Bus::kEngine)]) > 0.01);

      // Upshift triggers immediate Backfire pop (distinct from Burble).
      st.shifting = true;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(sfx.backfire_count() >= 1);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kBackfirePop);

      // Off-throttle overrun triggers Burble crackles cleanly.
      st.shifting = false;
      st.throttle = 0.0f;
      st.burble_min_time_ms = 40.0f;
      st.burble_max_time_ms = 60.0f;
      st.burble_silence_weight = 0.0f;
      for (int iter = 0; iter < 8; ++iter) {
        clear_bufs();
        sfx.render(st, ptrs, kFrames);
      }
      CHECK(sfx.burble_count() >= 1);

      // Multi-band EV renders when is_electric = true.
      st.is_electric = true;
      st.speed_mph = 70.0f;
      st.throttle = 0.8f;
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(rms(bufs[int(audio::Bus::kEngine)]) > 0.02);
    }

    // 5c. SurfaceChanges.xml gain/pitch multipliers + Leaves_On + distinct UI cues (kLap, kWristbandUnlock, kRewindTapeStop, kCountdownTick).
    {
      audio::SfxEngine sfx(48000);
      audio::TiresConfig tc;
      CHECK(tc.parse_tires(
          R"(<Tires><SurfaceMap PhysicsSurface="Asphalt" SurfaceIndex="0" Lod1EventGroup="OnRoad"/><SurfaceMap PhysicsSurface="Leaves" SurfaceIndex="51" Lod1EventGroup="Grass"/></Tires>)"));
      CHECK(tc.parse_surface_changes(
          R"(<SurfaceChanges><Surfaces Surface1="Asphalt" Surface2="Leaves"><S1toS2 Event="Leaves_On" Gain="1.4" Pitch="1.2"/></Surfaces></SurfaceChanges>)"));
      sfx.set_tires_config(tc);
      sfx.set_sample(audio::SfxSoundId::kSurfaceLeavesOn, make_tone(420.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kUiLap, make_tone(550.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kUiCheckpoint, make_tone(330.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kUiWristbandUnlock, make_tone(880.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kUiMenuOpen, make_tone(220.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kRewindTapeStop, make_tone(190.0, 2400));
      sfx.set_sample(audio::SfxSoundId::kCountdown321, make_tone(440.0, 48000));
      sfx.set_sample(audio::SfxSoundId::kUiCountdownTick, make_tone(980.0, 1200));

      audio::VehicleAudioState st{};
      st.speed_mph = 60.0f;
      for (auto& w : st.wheels) {
        w.surface_group = 0;
        w.surface_index = 0;
        w.load = 1.0f;
      }
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      for (auto& w : st.wheels) {
        w.surface_group = 2;
        w.surface_index = 51;
      }
      clear_bufs();
      sfx.render(st, ptrs, kFrames);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kSurfaceLeavesOn);
      CHECK(near(sfx.last_one_shot_pitch(), 1.2, 1e-3));

      // Distinct UI cues.
      sfx.trigger_ui(audio::UiCue::kLap);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kUiLap);
      sfx.trigger_ui(audio::UiCue::kWristbandUnlock);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kUiWristbandUnlock);
      sfx.trigger_ui(audio::UiCue::kRewindTapeStop);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kRewindTapeStop);

      // Countdown tick is suppressed while full kCountdown321 track is playing (prevents 3x stacking).
      sfx.trigger_ui(audio::UiCue::kCountdown321);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kCountdown321);
      sfx.trigger_ui(audio::UiCue::kCountdownTick);
      CHECK(sfx.last_one_shot_id() == audio::SfxSoundId::kCountdown321);
    }
  }

  // 6. CoDriverVoPlayer queue clear & gate supersede.
  {
    audio::CoDriverVoPlayer cd(48000);
    cd.set_clip("left3", make_tone(500.0, 4800));
    cd.set_clip("right2", make_tone(700.0, 4800));
    cd.set_clip("hairpin", make_tone(900.0, 4800));
    cd.enqueue_cue("left3");
    cd.enqueue_cue("right2");
    cd.enqueue_cue("left3");
    std::vector<float> cd_buf(64 * 2, 0.0f);
    CHECK(cd.render(cd_buf.data(), 64));
    CHECK(cd.playing() && cd.pending_count() == 2);
    // Newer gate supersedes pending queue cleanly (drops both stale queued clips and queues only "hairpin").
    cd.supersede_cue("hairpin");
    CHECK(cd.playing() && cd.pending_count() == 1);
    // Rewind/clear drops both active and pending clips.
    cd.clear();
    CHECK(!cd.playing() && cd.pending_count() == 0);
  }
}

// PT-10 end-to-end regression: full AudioEngine pipeline initialized via
// load_mixer_configs (retail-like AudioMixerSnapshots.xml + GlobalDSPConfig.xml +
// AudioEngineConfig.xml), default bus sliders, Loading/UIExterior/Paused -> FreeRoam
// transitions, and audible RMS > 0.01 on Engine, Radio/Music, SFX, UI, and FMV paths.
void playtest_pt10_audible_pipeline_checks() {
  namespace fs = std::filesystem;
  const fs::path tmp_root = fs::temp_directory_path() / "shift-pt10-audio-e2e";
  std::error_code ec;
  fs::remove_all(tmp_root, ec);
  fs::create_directories(tmp_root / "media" / "audio", ec);
  fs::create_directories(tmp_root / "music", ec);

  {
    std::ofstream f(tmp_root / "media" / "audio" / "AudioMixerSnapshots.xml");
    f << R"xml(<?xml version="1.0" encoding="utf-8"?>
<MixSnapshots>
  <DynamicsData>
    <MusicHighpass fadeinrate="3.0" fadeoutrate="33.0" zonefilterlevel="200.0"/>
    <CrashData threshold="50" drop="10"/>
    <TopSpeedData threshold="0.70"/>
  </DynamicsData>
  <FreeRoam group="Gameplay">
    <FocusCar active="1" destvolume="0.35" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <OppCar active="1" destvolume="0.35" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Tire active="1" destvolume="0.30" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <CarWind active="1" destvolume="0.30" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Collision active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <AmbientStatic active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <HomespaceMusic active="1" destvolume="0.8" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <MenuMusic active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameUI active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <VO active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <SatNav active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <DJModifier active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
  </FreeRoam>
  <Loading group="Gameplay">
    <FocusCar active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <OppCar active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Tire active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Collision active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="0.25" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <MenuMusic active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameUI active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
  </Loading>
  <UIExterior group="Gameplay">
    <FocusCar active="1" destvolume="0.175" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <OppCar active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Tire active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Collision active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="0.22" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <HomespaceMusic active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <MenuMusic active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameUI active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
  </UIExterior>
  <Paused group="Gameplay">
    <FocusCar active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <OppCar active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Tire active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <Collision active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameMusic active="1" destvolume="0.25" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <MenuMusic active="1" destvolume="0.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
    <GameUI active="1" destvolume="1.0" fadetime="0.05" returntime="0" returnfadetime="0"/>
  </Paused>
  <LowSpeed group="Speed"><GameMusic active="1" destvolume="1.0"/></LowSpeed>
  <AverageSpeedExit group="SpeedZones"><GameMusic active="1" destvolume="1.0"/></AverageSpeedExit>
  <VONotPlaying group="VO"><FocusCar active="1" destvolume="1.0"/></VONotPlaying>
  <NotInTunnel group="Reverb"><FocusCar active="1" destvolume="1.0"/></NotInTunnel>
  <RadioNormal group="Radio"><FocusCar active="1" destvolume="1.0"/></RadioNormal>
  <SatNavNotPlaying group="SatNav"><GameMusic active="1" destvolume="1.0"/></SatNavNotPlaying>
</MixSnapshots>)xml";
  }
  {
    std::ofstream f(tmp_root / "media" / "audio" / "GlobalDSPConfig.xml");
    f << R"xml(<GlobalDSPConfig>
  <RewindDSPConfig Bypass="false" Gain="0.30" Bandwidth="3.36" Center="3772.0"/>
  <ThreeTwoOneDSPConfig Bypass="false" Gain="0.10" Bandwidth="2.0" Center="1500.0"/>
  <CockpitDSPConfig Bypass="true" Bypass1="true" Bypass2="true" Bypass3="true"/>
</GlobalDSPConfig>)xml";
  }
  {
    std::ofstream f(tmp_root / "media" / "audio" / "AudioEngineConfig.xml");
    f << R"xml(<AudioEngineConfig
  compressorBypass="false" compressorLevel="1.0" compressorThreshold="0.1"
  compressorAttack="0.05" compressorRelease="0.5" compressorRatio="0.25" compressorGainMakeup="1.0"
  compressorBypassTV="false" compressorLevelTV="1.0" compressorThresholdTV="0.1"
  compressorAttackTV="0.05" compressorReleaseTV="0.5" compressorRatioTV="0.15" compressorGainMakeupTV="1.0"
  saturatorBypass="false" saturatorLevel="0.95" saturatorSaturation="0.90" saturatorGain="1.0"
/>)xml";
  }
  write_tone_wav(tmp_root / "music" / "01_radio_like.wav", 48000, 48000, 330.0, 0.5);

  auto rms_of = [](const std::vector<float>& pcm, size_t start_sample = 0) {
    double sq = 0.0;
    size_t n = 0;
    for (size_t i = start_sample; i < pcm.size(); ++i) {
      if (!std::isfinite(pcm[i])) return -1.0;
      sq += double(pcm[i]) * double(pcm[i]);
      ++n;
    }
    return n > 0 ? std::sqrt(sq / double(n)) : 0.0;
  };
  auto make_tone = [](double hz, int n, float amp = 0.6f) {
    audio::PcmSample s;
    s.rate = 48000;
    s.data.resize(size_t(n));
    for (int i = 0; i < n; ++i) s.data[size_t(i)] = amp * float(std::sin(2.0 * 3.14159265358979 * hz * i / 48000.0));
    return s;
  };

  auto init_engine = [&](audio::AudioEngine& eng) {
    std::string note;
    CHECK(eng.load_mixer_configs(tmp_root, &note));
    CHECK(eng.has_snapshot_matrix());
    eng.set_master_volume(1.0f);
    eng.set_mute(false);
    eng.set_playback_system(audio::PlaybackSystem::kHomeTheater);
    eng.set_full_dynamic_range(false);
    for (int b = 0; b < audio::kUiSliderBuses; ++b) eng.set_bus_level(audio::Bus(b), 1.0f);
    eng.activate_matrix_snapshot("FreeRoam");
  };

  // 1. Verify Loading -> FreeRoam, UIExterior -> FreeRoam, and Paused -> FreeRoam never trap buses in silence.
  {
    audio::AudioEngine eng(48000);
    init_engine(eng);
    std::vector<float> scratch(4800 * 2, 0.0f);
    eng.activate_matrix_snapshot("Loading");
    eng.render_offline(scratch.data(), 4800);  // 100 ms > 50 ms fade
    CHECK(eng.matrix_bus_gain(audio::Bus::kEngine) < 0.05f);
    eng.restore_gameplay_snapshot();
    eng.render_offline(scratch.data(), 4800);
    CHECK(eng.matrix_bus_gain(audio::Bus::kEngine) > 0.30f);

    // UIExterior must be transient and restore FreeRoam (including Tire/Collision/GameMusic)!
    eng.activate_matrix_snapshot("UIExterior");
    eng.render_offline(scratch.data(), 4800);
    CHECK(eng.matrix_bus_gain(audio::Bus::kTyres) < 0.05f);
    CHECK(eng.matrix_bus_gain(audio::Bus::kMusic) < 0.30f);
    eng.restore_gameplay_snapshot();
    eng.render_offline(scratch.data(), 4800);
    CHECK(eng.matrix_bus_gain(audio::Bus::kEngine) > 0.30f);
    CHECK(eng.matrix_bus_gain(audio::Bus::kTyres) > 0.25f);
    CHECK(eng.matrix_bus_gain(audio::Bus::kMusic) > 0.90f);

    eng.activate_matrix_snapshot("Paused");
    eng.render_offline(scratch.data(), 4800);
    eng.restore_gameplay_snapshot();
    eng.render_offline(scratch.data(), 4800);
    CHECK(eng.matrix_bus_gain(audio::Bus::kEngine) > 0.30f);
    CHECK(eng.matrix_bus_gain(audio::Bus::kMusic) > 0.90f);
  }

  // 2. Engine path produces audible RMS > 0.01 after Loading -> FreeRoam.
  {
    audio::AudioEngine eng(48000);
    init_engine(eng);
    audio::EngineSound es;
    audio::Group g;
    g.name = "EngineAmbient";
    g.loops.push_back(loop(800, 2000, 8000));
    es.groups.push_back(g);
    audio::PcmSample eng_tone = make_tone(120.0, 4800, 0.7f);
    auto ep = std::make_unique<audio::EnginePlayer>(
        es, std::vector<std::vector<const audio::PcmSample*>>{{&eng_tone}}, 48000, 800, 7500);
    ep->master = 0.6;
    eng.set_engine_player(std::move(ep));
    eng.set_engine_input({2200.0, 0.5, 0.4, 0.0, audio::View::kFollow});
    eng.activate_matrix_snapshot("Loading");
    std::vector<float> buf(9600 * 2, 0.0f);
    eng.render_offline(buf.data(), 4800);
    eng.restore_gameplay_snapshot();
    eng.render_offline(buf.data(), 9600);
    CHECK(rms_of(buf, 4800 * 2) > 0.01);
  }

  // 3. Music / Radio bus path produces audible RMS > 0.01 in FreeRoam.
  {
    audio::AudioEngine eng(48000);
    init_engine(eng);
    auto um = std::make_unique<audio::UserMusicPlayer>(tmp_root / "music", 48000, 7, false);
    um->set_enabled(true);
    um->set_volume(1.0f);
    eng.set_user_music(std::move(um));
    std::vector<float> buf(9600 * 2, 0.0f);
    eng.render_offline(buf.data(), 9600);
    CHECK(rms_of(buf, 2400 * 2) > 0.01);
  }

  // 4. SFX (collision) and UI cue paths each produce audible RMS > 0.01 in FreeRoam.
  {
    audio::AudioEngine eng(48000);
    init_engine(eng);
    auto sfx = std::make_unique<audio::SfxEngine>(48000);
    sfx->set_sample(audio::SfxSoundId::kImpact3d, make_tone(180.0, 9600, 0.8f));
    sfx->set_sample(audio::SfxSoundId::kUiAccept, make_tone(880.0, 9600, 0.8f));
    eng.set_sfx_engine(std::move(sfx));

    audio::CollisionHit hit{};
    hit.rel_speed_mps = 18.0f;
    hit.is_car_on_car = true;
    eng.trigger_collision(hit);
    std::vector<float> sfx_buf(4800 * 2, 0.0f);
    eng.render_offline(sfx_buf.data(), 4800);
    CHECK(rms_of(sfx_buf) > 0.01);

    // Silence collision tail then verify UI cue independently.
    std::vector<float> drain(14400 * 2, 0.0f);
    eng.render_offline(drain.data(), 14400);
    eng.trigger_ui(audio::UiCue::kAccept);
    std::vector<float> ui_buf(4800 * 2, 0.0f);
    eng.render_offline(ui_buf.data(), 4800);
    CHECK(rms_of(ui_buf) > 0.01);
  }

  // 5. FMV audio callback produces audible RMS > 0.01 even under matrix mode.
  {
    audio::AudioEngine eng(48000);
    init_engine(eng);
    struct FmvGen {
      int phase = 0;
    } gen;
    eng.set_fmv_audio(
        [](void* ctx, float* out, int frames) -> int {
          auto* g = static_cast<FmvGen*>(ctx);
          for (int i = 0; i < frames; ++i) {
            const float s = 0.4f * std::sin(2.0f * 3.14159265f * 440.0f * float(g->phase++) / 48000.0f);
            out[2 * i] = s;
            out[2 * i + 1] = s;
          }
          return frames;
        },
        &gen);
    std::vector<float> fmv_buf(4800 * 2, 0.0f);
    eng.render_offline(fmv_buf.data(), 4800);
    CHECK(rms_of(fmv_buf) > 0.01);
    eng.set_fmv_audio(nullptr, nullptr);
  }

  fs::remove_all(tmp_root, ec);
}

}  // namespace

int main() {
  rule_checks();
  xml_checks();
  upgrade_level_checks();
  fsb_checks();
  player_checks();
  rt_checks();
  mixer_checks();
  snapshot_and_master_dsp_checks();
  fev_and_event_config_checks();
  sfx_engine_checks();
  user_music_checks();
  audio_thread_checks();
  hitch_and_underrun_recovery_checks();
  phase2_parity_checks();
  phase_r2_1_audio_parity_checks();
  phase_r2_5_audio_completeness_checks();
  playtest_pt10_audible_pipeline_checks();
  r34_checks();
  std::printf("audio_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}

