// SPEC-ENGINE-SOUND: the synthetic rule checks (no game data), the reader
// and the player, then the R34's loops at 4,000 and 7,000 rpm over the
// player's files when SHIFT_GAME_ROOT names the disc.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/engine_player.h"
#include "audio/fsb4.h"
#include "audio/sources.h"
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
}

}  // namespace

int main() {
  rule_checks();
  xml_checks();
  fsb_checks();
  player_checks();
  r34_checks();
  std::printf("audio_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
