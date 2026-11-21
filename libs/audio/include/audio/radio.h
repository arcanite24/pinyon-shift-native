// The free-roam radio (SPEC-RADIO): the stations, playlists, idents and DJ
// lines of RadioSystem.xml, each clip resolved through its sound-bank info
// file; a schedule per station (track, ident at the track's IdentStart, the
// next track at the ident's OffsetStartNextTrack, a DJ line over the next
// track's intro); the station cycle on the D-pad, every station's clock
// running while the radio is on; and the popup's timing.
// Spec: SPEC-RADIO "Tracks", "Playback", "Mixing", "HUD".
// Ours (the spec's interims): uniform shuffle outside the last noRepeat
// picks; a DJ regular line after each ident when one ends before the next
// track's EventStart; the station-change fades run together; Bass Arena
// first.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace audio {

// One clip of a bank: its sample index and its sync points, in seconds.
struct RadioClip {
  uint32_t sample = 0;
  double length = 0;       // FileLength
  double event_start = 0;  // OffsetEventStart: end of a track's intro
  double ident_start = 0;  // OffsetIdentStart: a track's ident starts here
  double next_track = 0;   // OffsetStartNextTrack: an ident's next track
};

struct RadioTrack {
  std::string title, artist, name;
  int clip = -1;  // into RadioData::music
};

struct RadioStation {
  std::string name;
  bool off = false;
  std::vector<RadioTrack> tracks;
  std::vector<int> idents, dj;  // into RadioData::vo
  int no_repeat_tracks = 0, no_repeat_idents = 0, no_repeat_dj = 0;
};

// The System element's levels and times.
struct RadioLevels {
  double master = 1, music = 0.85, ident = 0.3, dialogue = 1;
  double duck = 0.3, duck_down = 0.1, duck_up = 0.5;
  double change_fade = 0.25;      // stationChangeFadeTimeUser
  double new_track_delay = 2.0;   // HUDNewTrackDisplayDelayFreeRoam
};

struct RadioData {
  RadioLevels levels;
  std::vector<RadioStation> stations;
  std::vector<RadioClip> music, vo;
  std::filesystem::path music_bank, vo_bank;
  int unresolved = 0;  // names no info entry gives
  // The D-pad cycle: the player stations (not off, not the 3D location
  // stations) in file order, then the off station.
  std::vector<int> cycle;
};

// Parses RadioSystem.xml and the two info files' text; `music_info` and
// `vo_info` resolve track and VO names.
bool parse_radio(std::string_view system_xml, std::string_view music_info, std::string_view vo_info, RadioData& out,
                 std::string* error);
// Reads media/audio/radio under `game_root` (the VO bank of `language`
// when it exists, else EN).
bool load_radio(const std::filesystem::path& game_root, RadioData& out, std::string* error,
                std::string_view language = "EN");

// A shuffle outside the last `no_repeat` picks (fewer when the list is short).
class RadioPicker {
 public:
  void reset(int count, int no_repeat) { count_ = count, no_repeat_ = no_repeat, recent_.clear(); }
  // A pick among those `allow` accepts (all when null); -1 if none.
  template <class Allow>
  int pick(std::mt19937_64& rng, Allow allow);
  int pick(std::mt19937_64& rng) {
    return pick(rng, [](int) { return true; });
  }

 private:
  int count_ = 0, no_repeat_ = 0;
  std::deque<int> recent_;
};

enum class RadioKind { kTrack, kIdent, kDj };

struct RadioEvent {
  RadioKind kind = RadioKind::kTrack;
  int clip = -1;   // into music (tracks) or vo
  int track = -1;  // the station's track entry, for a track
  double start = 0, end = 0;  // on the radio clock, s
  uint64_t id = 0;            // unique per scheduled clip
};

// One station's clips on the radio clock: it starts on a random track at a
// random point (time `t0`) and extends itself as it is asked about later
// times.
class StationSchedule {
 public:
  StationSchedule(const RadioData& data, int station, uint64_t seed, double t0, bool dj = true);
  // The clips sounding in [t0, t1), oldest first; clips that ended before
  // t0 are dropped (ask about times in order).
  void audible(double t0, double t1, std::vector<RadioEvent>& out);
  // The track playing at t (the latest started) and how far into it.
  const RadioEvent* track_at(double t, double* offset = nullptr);

 private:
  void extend();  // schedules the ident, the next track and a DJ line
  const RadioData& data_;
  const RadioStation& st_;
  std::mt19937_64 rng_;
  RadioPicker tracks_, idents_, dj_;
  bool use_dj_ = true;
  std::deque<RadioEvent> events_;
  const RadioEvent* last_track() const;
  uint64_t next_id_ = 0, station_ = 0;
};

// The radio: the selected station in the cycle, one clock for every
// station that runs while the radio is on, and the popup.
class Radio {
 public:
  Radio(const RadioData& data, uint64_t seed, bool dj = true);
  const RadioData& data() const { return data_; }
  int selected() const { return data_.cycle.empty() ? -1 : data_.cycle[size_t(pos_)]; }
  bool on() const;
  // D-pad right (+1) or left (-1).
  void press(int dir);
  // Starts on this station (an index into data().stations), no popup.
  void select(int station);
  // Advances the clock (while on, or with `run`: a station still fading
  // out) and the popup by `dt` seconds of output.
  void advance(double dt, bool run = false);
  double clock() const { return clock_; }
  StationSchedule* schedule(int station);

  // The popup: the station and track it names, and its opacity (0: none).
  struct Popup {
    int station = -1, track = -1;
    double alpha = 0;
  };
  Popup popup() const;
  static constexpr double kPopupDelay = 1.0, kPopupShow = 4.0, kPopupFade = 0.2;

 private:
  const RadioData& data_;
  std::vector<std::unique_ptr<StationSchedule>> schedules_;
  int pos_ = 0;
  double clock_ = 0;
  // The popup's timer: it shows from `popup_from` for kPopupShow seconds
  // (on the popup clock, which runs whether the radio is on or not).
  double popup_clock_ = 0, popup_from_ = -1e9;
  int popup_station_ = -1;
  uint64_t seen_track_ = ~0ull;  // the selected station's track last seen
};

// Plays a Radio: decodes the selected station's clips from the banks (MPEG
// frames, padding dropped, dr_mp3), resampled to the output rate, with the
// levels, the DJ duck and the station-change fades.
class RadioPlayer {
 public:
  RadioPlayer(Radio& radio, int out_rate);
  ~RadioPlayer();
  // Adds `frames` stereo frames into `out` (interleaved; adds, so mix the
  // engine first) and advances the radio.
  void render(float* out, int frames);
  double master = 1;  // ours: the radio against the engine
  int decoders() const;

 private:
  struct Decoder;
  struct Banks;
  Decoder* decoder(const RadioEvent& e);
  std::unique_ptr<Banks> banks_;
  Radio& radio_;
  int rate_;
  std::vector<std::unique_ptr<Decoder>> decoders_;
  std::vector<double> station_gain_;
  double duck_ = 1;
  std::vector<RadioEvent> scratch_;
  std::vector<float> mix_;
};

// The MPEG audio frames of `bytes`, with anything between them (padding)
// dropped; `frame_at` receives each kept frame's offset in the result.
std::vector<uint8_t> mpeg_frames(const uint8_t* bytes, size_t size, std::vector<uint32_t>* frame_at = nullptr,
                                 int* rate = nullptr, int* samples_per_frame = nullptr);

template <class Allow>
int RadioPicker::pick(std::mt19937_64& rng, Allow allow) {
  if (count_ <= 0) return -1;
  const size_t window = size_t(std::max(0, std::min(no_repeat_, count_ - 1)));
  std::vector<int> pool, any;
  for (int i = 0; i < count_; ++i) {
    if (!allow(i)) continue;
    any.push_back(i);
    bool recent = false;
    for (size_t k = 0; k < recent_.size() && k < window; ++k) recent |= recent_[recent_.size() - 1 - k] == i;
    if (!recent) pool.push_back(i);
  }
  if (pool.empty()) pool = any;
  if (pool.empty()) return -1;
  const int p = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];
  recent_.push_back(p);
  while (recent_.size() > window + 1) recent_.pop_front();
  return p;
}

}  // namespace audio
