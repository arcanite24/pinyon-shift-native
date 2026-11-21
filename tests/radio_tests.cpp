// The radio (SPEC-RADIO): parsing and name resolution, the schedule's
// ident and next-track points, the background rule, the cycle and the
// popup's timing on made-up data; with SHIFT_GAME_ROOT, the counts read
// from the player's disc and a short decode of the selected station.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <filesystem>
#include <fstream>

#include "audio/fsb4.h"
#include "audio/radio.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// Made-up stations: "Alpha" with one 200 s track (ident at 198 s), "Beta"
// with two, the off station and a location station left out of the cycle.
const char* kSystem = R"(<?xml version="1.0"?>
<RadioSystem>
  <System musicLevel="0.5" identLevel="0.25" duckValue="0.4" stationChangeFadeTimeUser="0.3"
          HUDNewTrackDisplayDelayFreeRoam="2.0"/>
  <RadioStation name="Alpha">
    <Playlist noRepeat="3">
      <MusicTrack name="First Song" artist="Band & Friends" fmodname="T_One" likelihoodDaytime="1"/>
    </Playlist>
    <Idents noRepeat="1"><Ident fmodName="I_One"/></Idents>
    <DJRegularDialogue noRepeat="1"><SimpleDialogue fmodName="D_Long"/></DJRegularDialogue>
  </RadioStation>
  <RadioStation name="Beta">
    <Playlist noRepeat="1">
      <MusicTrack name="Two" artist="X" fmodname="T_Two"/>
      <MusicTrack name="Three" artist="Y" fmodname="T_Three"/>
      <MusicTrack name="Missing" artist="Z" fmodname="T_Missing"/>
    </Playlist>
    <Idents noRepeat="1"><Ident fmodName="I_One"/></Idents>
    <DJRegularDialogue noRepeat="1"><SimpleDialogue fmodName="D_Short"/></DJRegularDialogue>
  </RadioStation>
  <RadioStation name="Gamma_Silent" isOffStation="true">
    <Playlist noRepeat="1"><MusicTrack name="None" artist="None" fmodName="T_Blank"/></Playlist>
  </RadioStation>
  <RadioStation name="Delta_3d_Hub" isOffStation="false" volume="0.3" 3dPanLevel="1.0">
    <Playlist noRepeat="1"><MusicTrack name="Hub" artist="H" fmodname="T_Two"/></Playlist>
  </RadioStation>
</RadioSystem>)";

const char* kMusicInfo = R"(<SoundBanks>
  <Bank Name="T_Blank"><SoundBankInfo SoundBankIndex="0" OffsetEventStart="0" OffsetIdentStart="0" FileLength="60000"/></Bank>
  <Bank Name="T_One"><SoundBankInfo SoundBankIndex="1" OffsetEventStart="30000" OffsetIdentStart="198000" FileLength="200000"/></Bank>
  <Bank Name="T_Two"><SoundBankInfo SoundBankIndex="2" OffsetEventStart="20000" OffsetIdentStart="95000" FileLength="100000"/></Bank>
  <Bank Name="T_Three"><SoundBankInfo SoundBankIndex="3" OffsetEventStart="25000" OffsetIdentStart="118000" FileLength="120000"/></Bank>
</SoundBanks>)";

const char* kVoInfo = R"(<SoundBanks>
  <Bank Name="I_One"><SoundBankInfo SoundBankIndex="0" OffsetStartNextTrack="6000" FileLength="8000"/></Bank>
  <Bank Name="D_Long"><SoundBankInfo SoundBankIndex="1" OffsetStartNextTrack="0" FileLength="40000"/></Bank>
  <Bank Name="D_Short"><SoundBankInfo SoundBankIndex="2" OffsetStartNextTrack="0" FileLength="10000"/></Bank>
</SoundBanks>)";

void parsing(const audio::RadioData& d) {
  CHECK(d.stations.size() == 4);
  CHECK(d.stations[0].tracks.size() == 1);
  CHECK(d.stations[0].tracks[0].artist == "Band & Friends");
  CHECK(d.stations[1].tracks.size() == 2);
  CHECK(d.unresolved == 1);  // T_Missing
  CHECK(d.stations[2].off);
  CHECK((d.cycle == std::vector<int>{0, 1, 2}));  // the 3D station is not in it
  CHECK(near(d.levels.music, 0.5) && near(d.levels.ident, 0.25) && near(d.levels.duck, 0.4));
  CHECK(near(d.levels.master, 1) && near(d.levels.change_fade, 0.3));
  const audio::RadioClip& one = d.music[size_t(d.stations[0].tracks[0].clip)];
  CHECK(one.sample == 1 && near(one.length, 200) && near(one.ident_start, 198) && near(one.event_start, 30));
}

// The spec's case: a 200 s track with IdentStart 198 s and an 8 s ident
// with OffsetStartNextTrack 6 s: the ident plays from 198 to 206 s, the
// next track starts at 204 s and the first ends at 200 s.
void sequencing(const audio::RadioData& d) {
  audio::StationSchedule s(d, 0, 7, 0.0);
  double offset = 0;
  const audio::RadioEvent* first = s.track_at(0, &offset);
  CHECK(first != nullptr);
  if (!first) return;
  const double s0 = first->start, first_end = first->end;
  CHECK(near(s0, -offset) && offset >= 0 && offset < 198);
  CHECK(near(first_end - s0, 200));
  std::vector<audio::RadioEvent> out;
  s.audible(s0 + 199, s0 + 205, out);
  const audio::RadioEvent *ident = nullptr, *next = nullptr, *dj = nullptr;
  for (const auto& e : out) {
    if (e.kind == audio::RadioKind::kIdent) ident = &e;
    if (e.kind == audio::RadioKind::kTrack && e.start > s0 + 1) next = &e;
    if (e.kind == audio::RadioKind::kDj) dj = &e;
  }
  CHECK(ident && near(ident->start, s0 + 198) && near(ident->end, s0 + 206));
  CHECK(next && near(next->start, s0 + 204));
  // The 40 s line doesn't fit before the 30 s EventStart: no DJ line.
  CHECK(dj == nullptr);
  // Beta's 10 s line fits both intros: a line starts with each next track.
  audio::StationSchedule b(d, 1, 9, 0.0);
  const audio::RadioEvent* bt = b.track_at(0);
  CHECK(bt != nullptr);
  if (!bt) return;
  const double end = bt->end;
  s.audible(0, 0, out);
  b.audible(end - 1, end + 30, out);
  bool line = false;
  for (const auto& e : out)
    if (e.kind == audio::RadioKind::kDj) {
      line = true;
      bool with_track = false;
      for (const auto& t : out) with_track |= t.kind == audio::RadioKind::kTrack && near(t.start, e.start);
      CHECK(with_track);
      CHECK(near(e.end - e.start, 10));
    }
  CHECK(line);
}

// Stations A and B: A for 30 s, off for 60 s, B for 20 s, then A: A is 20 s
// past where it was left; B is 50 s past where it stood when the radio
// first came on. One clock for both, stopped while off.
void background(const audio::RadioData& d) {
  audio::Radio r(d, 3);
  double at0 = 0;
  const audio::RadioEvent* b0 = r.schedule(1)->track_at(0, &at0);
  CHECK(b0 != nullptr);
  const uint64_t b0_id = b0 ? b0->id : 0;
  r.select(0);
  CHECK(r.on() && r.selected() == 0);
  r.advance(30);
  const double left_at = r.clock();
  r.select(2);  // off
  CHECK(!r.on());
  r.advance(60);
  CHECK(near(r.clock(), left_at));
  r.select(1);
  r.advance(20);
  r.select(0);
  CHECK(near(r.clock() - left_at, 20));
  CHECK(near(r.clock(), 50));
  // B's schedule is on the same clock: where it stood at 0, it is 50 s on.
  double at50 = 0;
  const audio::RadioEvent* now = r.schedule(1)->track_at(50, &at50);
  CHECK(now != nullptr);
  if (now && now->id == b0_id) CHECK(near(at50 - at0, 50));
  if (now && now->id != b0_id) CHECK(now->start > -at0 && now->start <= 50);
}

void cycle_and_popup(const audio::RadioData& d) {
  audio::Radio r(d, 5);
  r.select(2);  // off
  std::vector<int> seen;
  for (int i = 0; i < 4; ++i) r.press(+1), seen.push_back(r.selected());
  CHECK((seen == std::vector<int>{0, 1, 2, 0}));
  seen.clear();
  for (int i = 0; i < 2; ++i) r.press(-1), seen.push_back(r.selected());
  CHECK((seen == std::vector<int>{2, 1}));
  // A station's popup: after 1 s, for 4 s, fading over 0.2 s.
  audio::Radio p(d, 5);
  p.select(2);
  p.press(+1);  // Alpha
  p.advance(0.5);
  CHECK(p.popup().alpha == 0);
  p.advance(0.6);  // 1.1 s
  CHECK(p.popup().alpha > 0 && p.popup().alpha < 1 && p.popup().station == 0);
  p.advance(1.0);  // 2.1 s
  CHECK(near(p.popup().alpha, 1) && p.popup().track == 0);
  p.advance(3.0);  // 5.1 s
  CHECK(p.popup().alpha == 0);
  // RADIO OFF at once.
  p.press(-1);
  p.advance(0.1);
  CHECK(p.popup().alpha > 0 && p.popup().station == 2);
  p.advance(4.0);
  CHECK(p.popup().alpha == 0);
}

void picker() {
  std::mt19937_64 rng(11);
  audio::RadioPicker pk;
  pk.reset(5, 3);
  std::vector<int> history;
  bool ok = true;
  for (int i = 0; i < 2000; ++i) {
    const int x = pk.pick(rng);
    for (size_t k = 1; k <= 3 && k <= history.size(); ++k) ok &= history[history.size() - k] != x;
    history.push_back(x);
  }
  CHECK(ok);
  // A window as long as the list: no immediate repeat, still picks.
  pk.reset(2, 15);
  int last = -1;
  ok = true;
  for (int i = 0; i < 100; ++i) {
    const int x = pk.pick(rng);
    ok &= x != last && x >= 0;
    last = x;
  }
  CHECK(ok);
}

// Two made-up MPEG-1 Layer III frames (160 kbps, 48 kHz: 480 bytes) with
// zero padding between them.
void frames() {
  std::vector<uint8_t> bytes;
  auto frame = [&]() {
    const size_t at = bytes.size();
    bytes.resize(at + 480, 0x55);
    bytes[at] = 0xFF, bytes[at + 1] = 0xFB, bytes[at + 2] = 0xA4, bytes[at + 3] = 0x00;
  };
  frame();
  bytes.resize(bytes.size() + 37, 0);
  frame();
  bytes.resize(bytes.size() + 5, 0);
  std::vector<uint32_t> at;
  int rate = 0, spf = 0;
  const auto out = audio::mpeg_frames(bytes.data(), bytes.size(), &at, &rate, &spf);
  CHECK(out.size() == 960);
  CHECK((at == std::vector<uint32_t>{0, 480}));
  CHECK(rate == 48000 && spf == 1152);
}

void disc(const char* root) {
  audio::RadioData d;
  std::string error;
  if (!audio::load_radio(root, d, &error)) {
    std::printf("FAIL load_radio: %s\n", error.c_str());
    ++failures;
    return;
  }
  auto find = [&](const char* name) -> const audio::RadioStation* {
    for (const auto& s : d.stations)
      if (s.name == name) return &s;
    return nullptr;
  };
  const auto *r1 = find("Radio1"), *r2 = find("Radio2"), *r3 = find("Radio3"), *off = find("Radio4_Silent");
  CHECK(r1 && r2 && r3 && off);
  if (!r1 || !r2 || !r3 || !off) return;
  CHECK(r1->tracks.size() == 22 && r1->idents.size() == 11 && r1->dj.size() == 72);
  CHECK(r2->tracks.size() == 23 && r2->idents.size() == 10 && r2->dj.size() == 65);
  CHECK(r3->tracks.size() == 22 && r3->idents.size() == 14 && r3->dj.size() == 79);
  CHECK(off->off && off->tracks.size() == 2 && off->idents.size() == 6);
  CHECK(r1->no_repeat_tracks == 15 && r3->no_repeat_idents == 8 && r1->no_repeat_dj == 40);
  CHECK(d.unresolved == 0);
  CHECK(d.cycle.size() == 4);
  CHECK(d.stations[size_t(d.cycle[3])].off);
  // Decoded lengths (the spec's check): the first track and the first
  // ident of each player station, each within one frame of its header.
  auto headers = [](const std::filesystem::path& p, std::vector<audio::FsbSample>& out) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> h(48);
    f.read(reinterpret_cast<char*>(h.data()), 48);
    const uint32_t n = uint32_t(h[8]) | uint32_t(h[9]) << 8 | uint32_t(h[10]) << 16 | uint32_t(h[11]) << 24;
    h.resize(48 + size_t(n));
    f.read(reinterpret_cast<char*>(h.data() + 48), n);
    return audio::read_fsb4(h, out, nullptr, std::filesystem::file_size(p));
  };
  std::vector<audio::FsbSample> music, vo;
  CHECK(headers(d.music_bank, music) && headers(d.vo_bank, vo));
  int checked = 0, exact = 0;
  auto length = [&](const std::filesystem::path& bank, const audio::FsbSample& s) {
    std::ifstream f(bank, std::ios::binary);
    std::vector<uint8_t> raw(s.bytes);
    f.seekg(std::streamoff(s.offset));
    f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
    std::vector<uint32_t> at;
    int rate = 0, spf = 0;
    audio::mpeg_frames(raw.data(), raw.size(), &at, &rate, &spf);
    const int64_t got = int64_t(at.size()) * spf, want = s.frames;
    ++checked;
    exact += got >= want && got - want < spf;
    if (std::llabs(got - want) > 1152)
      std::printf("  %s: %lld samples in its frames, header %lld\n", s.name.c_str(), (long long)got, (long long)want);
    return std::llabs(got - want) <= 1152;
  };
  for (int k = 0; k < 3; ++k) {
    const auto& st = d.stations[size_t(d.cycle[size_t(k)])];
    CHECK(length(d.music_bank, music[d.music[size_t(st.tracks[0].clip)].sample]));
    CHECK(length(d.vo_bank, vo[d.vo[size_t(st.idents[0])].sample]));
  }
  std::printf("disc: %d of %d decoded lengths within a frame (whole frames)\n", exact, checked);
  // Two seconds of the first station: decoded and not silent.
  audio::Radio radio(d, 1);
  radio.select(d.cycle[0]);
  audio::RadioPlayer player(radio, 48000);
  std::vector<float> out(2 * 96000, 0.0f);
  player.render(out.data(), 48000);
  player.render(out.data() + 96000, 48000);
  double sum = 0;
  for (float x : out) sum += double(x) * x;
  const double rms = std::sqrt(sum / double(out.size()));
  std::printf("disc: Radio1 2 s RMS %.4f, %d decoders\n", rms, player.decoders());
  CHECK(rms > 0.01 && rms < 0.7);
  // Across a change: from 2 s before the track's ident, 24 s in quarter
  // seconds; the ident, the next track and any DJ line decode with no
  // dropout or burst.
  double into = 0;
  const audio::RadioEvent* now = radio.schedule(radio.selected())->track_at(radio.clock(), &into);
  CHECK(now != nullptr);
  if (!now) return;
  radio.advance(d.music[size_t(now->clip)].ident_start - 2 - into);
  int most = 0, quiet = 0;
  bool finite = true;
  double loudest = 0;
  std::vector<float> block(2 * 12000);
  for (int b = 0; b < 96; ++b) {
    std::fill(block.begin(), block.end(), 0.0f);
    player.render(block.data(), 12000);
    most = std::max(most, player.decoders());
    double s2 = 0;
    for (float x : block) finite &= std::isfinite(x), s2 += double(x) * x;
    const double r = std::sqrt(s2 / double(block.size()));
    quiet += r < 0.003;
    loudest = std::max(loudest, r);
  }
  std::printf("disc: across a change, up to %d clips at once, %d quiet quarter seconds, loudest %.3f RMS\n", most,
              quiet, loudest);
  CHECK(finite && most >= 2 && quiet <= 2 && loudest < 0.8);
}

}  // namespace

int main() {
  audio::RadioData d;
  std::string error;
  if (!audio::parse_radio(kSystem, kMusicInfo, kVoInfo, d, &error)) {
    std::printf("FAIL parse_radio: %s\n", error.c_str());
    return 1;
  }
  parsing(d);
  sequencing(d);
  background(d);
  cycle_and_popup(d);
  picker();
  frames();
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) disc(root);
  else std::printf("SHIFT_GAME_ROOT not set: disc checks skipped\n");
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("radio_tests: all passed\n");
  return failures ? 1 : 0;
}
