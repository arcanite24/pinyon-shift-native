// SPEC-ENGINE-SOUND: the synthetic rule checks (no game data), the reader
// and the player, then the R34's loops at 4,000 and 7,000 rpm over the
// player's files when SHIFT_GAME_ROOT names the disc.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/bake.h"
#include "audio/engine_player.h"
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
  user_music_checks();
  audio_thread_checks();
  r34_checks();
  std::printf("audio_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}

