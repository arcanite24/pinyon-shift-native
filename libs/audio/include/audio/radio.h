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
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
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
  double likelihood_day = 1.0;
  double likelihood_morning = 1.0;
  double likelihood_evening = 1.0;
  double likelihood_night = 1.0;
};

struct RadioStation {
  std::string name;
  bool off = false;
  bool is_3d = false;
  double volume = 1.0;
  double reverb_wet_level = 0.0;
  double pan_level_3d = 0.0;
  std::vector<RadioTrack> tracks;
  std::vector<int> idents, dj;  // into RadioData::vo
  std::vector<int> festival_lead_in, festival_lead_out;  // into RadioData::vo
  std::unordered_map<std::string, int> special_dialogue;    // lowercase gameplayEvent -> vo clip
  std::unordered_map<std::string, int> immediate_dialogue;  // lowercase gameplayEvent -> vo clip
  int no_repeat_tracks = 0, no_repeat_idents = 0, no_repeat_dj = 0;
  int no_repeat_lead_in = 0, no_repeat_lead_out = 0;
};

struct RadioVoxPops {
  std::vector<std::string> events;
  int no_repeat = 0;
  double interval_min_s = 3600.0;
  double interval_max_s = 48000.0;
};

// The System element's levels and times.
struct RadioLevels {
  double master = 1, music = 0.85, ident = 0.3, dialogue = 1;
  double duck = 0.3, duck_down = 0.1, duck_up = 0.5;
  double change_fade = 0.25;         // stationChangeFadeTimeUser
  double change_fade_normal = 1.0;   // stationChangeFadeTimeNormal
  double new_track_delay = 2.0;      // HUDNewTrackDisplayDelayFreeRoam
  int max_tracks_between_dj = 1;  // maxTracksBetweenDJ
};

struct RadioData {
  RadioLevels levels;
  std::vector<RadioStation> stations;
  std::vector<RadioClip> music, vo;
  std::unordered_map<std::string, int> festival_updates;  // lowercase gameplayEvent -> vo clip
  RadioVoxPops vox_pops;
  std::filesystem::path music_bank, vo_bank;
  std::string language = "EN";  // resolved VO language code
  int unresolved = 0;           // names no info entry gives
  // The D-pad cycle: the player stations (not off, not the 3D location
  // stations) in file order, then the off station.
  std::vector<int> cycle;
  // The 3D location stations (Radio5_3d_PreRace..Radio9_3d_Streetrace).
  std::vector<int> location_stations;
};

// Parses RadioSystem.xml and the two info files' text; `music_info` and
// `vo_info` resolve track and VO names.
bool parse_radio(std::string_view system_xml, std::string_view music_info, std::string_view vo_info, RadioData& out,
                 std::string* error);
// Parses RadioDJVoxPops.xml.
bool parse_radio_vox_pops(std::string_view vox_pops_xml, RadioVoxPops& out, std::string* error = nullptr);
// Reads media/audio/radio under `game_root` (honouring cars::mods::resolve;
// uses `language`'s VO bank when it exists, else falls back to EN).
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
  // Weighted pick respecting the `no_repeat` window (falls back to all positive-weight
  // candidates when all are recent, or uniform pick when all weights are <= 0).
  template <class WeightFn>
  int pick_weighted(std::mt19937_64& rng, WeightFn weight_fn);

 private:
  int count_ = 0, no_repeat_ = 0;
  std::deque<int> recent_;
};

enum class RadioKind { kTrack, kIdent, kDj, kFestivalUpdate };

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
  void set_dj_enabled(bool on) { use_dj_ = on; }
  bool dj_enabled() const { return use_dj_; }
  void set_time_of_day_hour(double hour) { tod_hour_ = hour; }

  // Gameplay-triggered radio dialogue (Slice 9):
  // - Special dialogue (barn-find / location hints) plays over next track intro.
  // - Immediate dialogue (story lines) starts right away at `now_clock`.
  // - Festival update queues LeadIn -> Update -> LeadOut over next track intro.
  bool queue_special_dialogue(std::string_view gameplay_event);
  bool trigger_immediate_dialogue(std::string_view gameplay_event, double now_clock);
  bool queue_festival_update(std::string_view gameplay_event);
  // Drops the festival updates queued and not yet scheduled
  // (radiodj.xml CancelPreviousTier).
  void cancel_festival_updates() { pending_festival_.clear(); }
  // Locks playback to an explicit ordered track list (e.g. Viper intro 2-track drive).
  // The first track starts from its beginning at `now_clock`; with
  // `then_normal` the station's own picks resume once the list has played
  // (CSetRadioPlaylist normalBehaviourAfterListPlayed), else the list loops.
  void set_locked_playlist(std::vector<int> track_indices, double now_clock, bool then_normal = false);
  void clear_locked_playlist();
  bool has_locked_playlist() const { return !locked_tracks_.empty(); }
  // Silent idents (Audio.enable_radio_silent_idents): an ident keeps its
  // place between tracks but plays nothing.
  void set_silent_idents(bool on) { silent_idents_ = on; }
  // Cuts the DJ line sounding at `now_clock` and drops the DJ lines queued
  // after it (the race to the festival's last stretch).
  void stop_dj(double now_clock);

 private:
  void ensure_extended(double t0, double t1);
  void extend();  // schedules the ident, the next track and a DJ line
  int pick_weighted_track();
  const RadioData& data_;
  const RadioStation& st_;
  std::mt19937_64 rng_;
  RadioPicker tracks_, idents_, dj_, lead_in_, lead_out_;
  bool use_dj_ = true;
  double tod_hour_ = 12.0;
  int tracks_since_dj_ = 0;
  std::vector<int> locked_tracks_;
  size_t locked_cursor_ = 0;
  bool locked_then_normal_ = false;
  bool silent_idents_ = false;
  std::deque<int> pending_special_;   // vo clip indices
  std::deque<int> pending_festival_;  // vo clip indices
  std::deque<RadioEvent> events_;
  const RadioEvent* last_track() const;
  uint64_t next_id_ = 0, station_ = 0;
};

struct LocationRadioEmitter {
  int station = -1;  // index into RadioData::stations (e.g. Radio5_3d_PreRace..Radio9_3d_Streetrace)
  std::array<float, 3> pos{};
  float min_distance = 12.0f;
  float max_distance = 220.0f;
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
  // Pass `user_change = true` when initiated by a user station-cycle press so
  // the fast `change_fade` (0.25 s) is used instead of `change_fade_normal`.
  void select(int station, bool user_change = false);
  bool last_change_was_user() const { return last_change_was_user_; }
  int find_station(std::string_view name) const;  // -1 if not found
  std::string selected_name() const;
  void set_dj_enabled(bool on);
  bool dj_enabled() const { return dj_enabled_; }
  void set_time_of_day_hour(double hour);
  bool trigger_special_dialogue(std::string_view gameplay_event);
  bool trigger_immediate_dialogue(std::string_view gameplay_event);
  bool trigger_festival_update(std::string_view gameplay_event);
  void cancel_festival_updates();
  // Locks Radio to the Viper opening drive 2-track Bass Arena playlist
  // ("Levels (Skrillex Remix)" -> "Language"), preventing D-pad station changes until unlocked.
  bool lock_viper_intro_playlist();
  bool lock_viper_intro_playlist(int station, std::vector<int> track_indices);
  void unlock_viper_intro_playlist();
  bool viper_intro_locked() const { return viper_intro_locked_; }
  // CSetRadioPlaylist: tunes to the station holding the first track (by
  // RadioSystem.xml fmodname) and plays the named tracks in order from the
  // first one's beginning; see StationSchedule::set_locked_playlist.
  bool set_playlist(const std::vector<std::string>& fmodnames, bool then_normal);
  void cancel_playlist();
  // Station switching (Audio.disable/enable_radio_station_switching): while
  // off the D-pad changes nothing.
  void set_switching_enabled(bool on) { switching_enabled_ = on; }
  bool switching_enabled() const { return switching_enabled_; }
  void set_silent_idents(bool on);
  void stop_dj();
  double location_clock() const { return location_clock_; }
  void advance_location_clock(double dt) {
    if (dt > 0.0) location_clock_ += dt;
  }
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
  // While the career's cutscenes hide the HUD: no popup, and a track that
  // starts meanwhile shows its popup after the new-track delay once the HUD
  // is back (the original's hand-over and race to the festival; Q273).
  void hold_popup(bool held);

 private:
  const RadioData& data_;
  std::vector<std::unique_ptr<StationSchedule>> schedules_;
  int pos_ = 0;
  bool dj_enabled_ = true;
  bool viper_intro_locked_ = false;
  bool switching_enabled_ = true;
  bool silent_idents_ = false;
  double clock_ = 0;
  double location_clock_ = 0;
  double since_festival_update_s_ = 0;
  double next_vox_pop_s_ = 3600.0;
  size_t vox_pop_rr_ = 0;
  // The popup's timer: it shows from `popup_from` for kPopupShow seconds
  // (on the popup clock, which runs whether the radio is on or not).
  double popup_clock_ = 0, popup_from_ = -1e9;
  int popup_station_ = -1;
  uint64_t seen_track_ = ~0ull;  // the selected station's track last seen
  bool popup_held_ = false, popup_deferred_ = false;
  bool last_change_was_user_ = true;
};

// Returns canonical Colorado 3D radio bleed emitters: Festival stages, Autoshow,
// Workshop, Paintshop, Street Race Hubs (Carson, Gladstone, Red Rock), and Outposts.
std::vector<LocationRadioEmitter> default_colorado_location_emitters(const Radio& radio);

// Plays a Radio: decodes the selected station's clips on a background
// StreamPool worker (MPEG frames, padding dropped, dr_mp3), resampled to the
// output rate, with prefetching, the levels, the DJ duck and station fades.
class RadioPlayer {
 public:
  RadioPlayer(Radio& radio, int out_rate, bool non_blocking = false);
  ~RadioPlayer();
  // Adds `frames` stereo frames into `out` (interleaved; adds, so mix the
  // engine first) and advances the radio.
  void render(float* out, int frames);
  void set_non_blocking(bool on) { non_blocking_ = on; }
  bool non_blocking() const { return non_blocking_; }
  void set_prefetch_seconds(double s) { prefetch_s_ = std::max(0.0, s); }
  double master = 1;  // ours: the radio against the engine
  double volume = 1;  // SPEC-RADIO: RADIO VOLUME option (0..1)
  void set_dj_modifier(double g) { dj_modifier_ = std::clamp(g, 0.0, 2.0); }
  double dj_modifier() const { return dj_modifier_; }
  int decoders() const;
  bool dj_speaking() const { return dj_speaking_; }
  bool festival_update_playing() const { return festival_update_playing_; }

  // 3D location station emitters (Radio5_3d_PreRace..Radio9_3d_Streetrace).
  void set_location_emitters(std::vector<LocationRadioEmitter> emitters);
  void add_location_emitter(LocationRadioEmitter emitter);
  const std::vector<LocationRadioEmitter>& location_emitters() const { return emitters_; }
  void set_listener(std::array<float, 3> pos, float heading_rad);
  // Renders active 3D location stations near the listener into `out` (additive)
  // and unconditionally advances `radio_.location_clock()`.
  void render_location_stations(float* out, int frames);
  // Renders Homespace interior station (1=Autoshow/Radio6, 2=Paintshop/Radio8,
  // 3=Workshop/Radio7) into `out` (additive). Returns true when a matching
  // station exists and was rendered (even if still buffering).
  bool render_homespace_station(int homespace_mode, float* out, int frames);
  // Releases any active Homespace station voices (call when leaving interior).
  void stop_homespace_voices();

 private:
  static constexpr size_t kMaxVoices = 16;
  struct Voice {
    bool active = false;
    uint64_t id = 0;
    int slot = -1;
    bool used = false;
    double pos = 0.0;  // source frame of the next output frame
    float env = 0.0f;  // smooth fade envelope across seek/underrun boundaries (zero clicks)
    float last_l = 0.0f;
    float last_r = 0.0f;
    bool location = false;
    bool homespace = false;
  };
  struct EmitterFilter {
    float hp_prev_in = 0.0f;
    float hp_prev_out = 0.0f;
    float lp_state = 0.0f;
  };
  struct Banks;
  Voice* voice_for(uint64_t voice_id, const RadioEvent& e, int64_t initial_frame);
  Voice* voice_for(const RadioEvent& e, int64_t initial_frame) { return voice_for(e.id, e, initial_frame); }
  void release_voice(Voice& v);
  std::unique_ptr<Banks> banks_;
  Radio& radio_;
  int rate_;
  bool non_blocking_ = false;
  double prefetch_s_ = 6.0;
  std::array<Voice, kMaxVoices> voices_{};
  std::vector<double> station_gain_;
  double duck_ = 1;
  double dj_modifier_ = 1.0;
  bool dj_speaking_ = false;
  bool festival_update_playing_ = false;
  std::vector<LocationRadioEmitter> emitters_;
  std::vector<float> emitter_gain_;
  std::vector<EmitterFilter> emitter_filters_;
  std::array<float, 3> listener_pos_{};
  float listener_heading_ = 0.0f;
  std::vector<RadioEvent> scratch_;
  std::vector<float> mix_;
  std::vector<float> loc_mono_;
  float homespace_gain_ = 0.0f;
  int homespace_stations_[3]{-1, -1, -1};  // [0]=Autoshow(Radio6), [1]=Paintshop(Radio8), [2]=Workshop(Radio7)
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

template <class WeightFn>
int RadioPicker::pick_weighted(std::mt19937_64& rng, WeightFn weight_fn) {
  if (count_ <= 0) return -1;
  const size_t window = size_t(std::max(0, std::min(no_repeat_, count_ - 1)));
  auto is_recent = [&](int idx) {
    for (size_t k = 0; k < recent_.size() && k < window; ++k) {
      if (recent_[recent_.size() - 1 - k] == idx) return true;
    }
    return false;
  };
  double total_fresh = 0.0;
  double total_any = 0.0;
  for (int i = 0; i < count_; ++i) {
    const double w = std::max(0.0, double(weight_fn(i)));
    if (w <= 0.0) continue;
    total_any += w;
    if (!is_recent(i)) total_fresh += w;
  }
  if (total_any <= 0.0) return pick(rng);
  const bool use_fresh = total_fresh > 0.0;
  const double total = use_fresh ? total_fresh : total_any;
  const double target = std::uniform_real_distribution<double>(0.0, total)(rng);
  double acc = 0.0;
  int chosen = -1;
  for (int i = 0; i < count_; ++i) {
    if (use_fresh && is_recent(i)) continue;
    const double w = std::max(0.0, double(weight_fn(i)));
    if (w <= 0.0) continue;
    chosen = i;
    acc += w;
    if (acc >= target) break;
  }
  if (chosen < 0) return pick(rng);
  recent_.push_back(chosen);
  while (recent_.size() > window + 1) recent_.pop_front();
  return chosen;
}

}  // namespace audio
