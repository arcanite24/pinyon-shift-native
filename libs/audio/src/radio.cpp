#include "audio/radio.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "audio/fsb4.h"
#include "audio/stream.h"
#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

bool fail(std::string* error, std::string what) {
  if (error) *error = std::move(what);
  return false;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// An info file: names (lower case) to clips.
bool parse_info(std::string_view text, std::map<std::string, RadioClip>& out, std::string* error) {
  XmlNode root;
  if (!parse_xml(text, root, error)) return false;
  for (const auto& bank : root.children) {
    if (!iequals(bank.name, "Bank")) continue;
    const XmlNode* info = bank.child("SoundBankInfo");
    if (!info) continue;
    RadioClip c;
    c.sample = uint32_t(info->number("SoundBankIndex", -1));
    c.length = info->number("FileLength") / 1000;
    c.event_start = info->number("OffsetEventStart") / 1000;
    c.ident_start = info->number("OffsetIdentStart") / 1000;
    c.next_track = info->number("OffsetStartNextTrack") / 1000;
    if (info->number("SoundBankIndex", -1) < 0) continue;
    out[lower(bank.attr("Name"))] = c;
  }
  return true;
}

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

// MPEG audio frame headers (Layer III; versions 1, 2 and 2.5).
struct FrameHeader {
  int bytes = 0, rate = 0, samples = 0;
};
bool frame_header(const uint8_t* p, size_t avail, FrameHeader& h) {
  if (avail < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  const int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
  const int br = p[2] >> 4, sr = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
  if (version == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) return false;
  static constexpr int kBr1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static constexpr int kBr2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  static constexpr int kSr[3] = {44100, 48000, 32000};
  const bool v1 = version == 3;
  const int kbps = v1 ? kBr1[br] : kBr2[br];
  h.rate = kSr[sr] / (v1 ? 1 : version == 2 ? 2 : 4);
  h.samples = v1 ? 1152 : 576;
  h.bytes = (v1 ? 144 : 72) * kbps * 1000 / h.rate + pad;
  return h.bytes > 4;
}

}  // namespace

std::vector<uint8_t> mpeg_frames(const uint8_t* bytes, size_t size, std::vector<uint32_t>* frame_at, int* rate,
                                 int* samples_per_frame) {
  std::vector<uint8_t> out;
  out.reserve(size);
  if (frame_at) frame_at->clear();
  size_t at = 0;
  while (at + 4 <= size) {
    FrameHeader h;
    if (!frame_header(bytes + at, size - at, h) || at + size_t(h.bytes) > size) {
      ++at;
      continue;
    }
    // A false sync in the padding: the next frame must follow, or the data end.
    FrameHeader next;
    const size_t after = at + size_t(h.bytes);
    if (after + 4 <= size && !frame_header(bytes + after, size - after, next) &&
        std::any_of(bytes + after, bytes + std::min(size, after + 64), [](uint8_t b) { return b != 0; }) &&
        bytes[after] != 0) {
      ++at;
      continue;
    }
    if (rate) *rate = h.rate;
    if (samples_per_frame) *samples_per_frame = h.samples;
    if (frame_at) frame_at->push_back(uint32_t(out.size()));
    out.insert(out.end(), bytes + at, bytes + after);
    at = after;
  }
  return out;
}

bool parse_radio(std::string_view system_xml, std::string_view music_info, std::string_view vo_info, RadioData& out,
                 std::string* error) {
  std::map<std::string, RadioClip> music, vo;
  if (!parse_info(music_info, music, error) || !parse_info(vo_info, vo, error)) return false;
  XmlNode root;
  if (!parse_xml(system_xml, root, error)) return false;
  out = RadioData{};
  if (const XmlNode* s = root.child("System")) {
    RadioLevels& l = out.levels;
    l.master = s->number("masterLevel", l.master);
    l.music = s->number("musicLevel", l.music);
    l.ident = s->number("identLevel", l.ident);
    l.dialogue = s->number("dialogueLevel", l.dialogue);
    l.duck = s->number("duckValue", l.duck);
    l.duck_down = s->number("duckFadeDownTime", l.duck_down);
    l.duck_up = s->number("duckFadeUpTime", l.duck_up);
    l.change_fade = s->number("stationChangeFadeTimeUser", l.change_fade);
    l.change_fade_normal = s->number("stationChangeFadeTimeNormal", l.change_fade_normal);
    l.new_track_delay = s->number("HUDNewTrackDisplayDelayFreeRoam", l.new_track_delay);
    l.max_tracks_between_dj = std::max(1, int(s->number("maxTracksBetweenDJ", 1)));
  }
  // Clips are kept once per name.
  std::map<std::string, int> music_at, vo_at;
  auto resolve = [&](const std::string& name, std::map<std::string, RadioClip>& info, std::map<std::string, int>& at,
                     std::vector<RadioClip>& clips) {
    const std::string key = lower(name);
    if (auto it = at.find(key); it != at.end()) return it->second;
    auto found = info.find(key);
    if (found == info.end()) {
      ++out.unresolved;
      return -1;
    }
    clips.push_back(found->second);
    return at[key] = int(clips.size() - 1);
  };
  for (const auto& node : root.children) {
    if (iequals(node.name, "FestivalUpdates")) {
      for (const auto& fu : node.children) {
        if (!iequals(fu.name, "FestivalUpdate")) continue;
        const std::string ev = lower(fu.attr("gameplayEvent"));
        const int k = resolve(fu.attr("fmodName"), vo, vo_at, out.vo);
        if (!ev.empty() && k >= 0) out.festival_updates[ev] = k;
      }
      continue;
    }
    if (!iequals(node.name, "RadioStation")) continue;
    RadioStation st;
    st.name = node.attr("name");
    st.off = iequals(node.attr("isOffStation"), "true");
    st.is_3d = lower(st.name).find("_3d") != std::string::npos;
    st.volume = node.number("volume", 1.0);
    st.reverb_wet_level = node.number("reverbWetLevel", 0.0);
    st.pan_level_3d = node.number("3dPanLevel", st.is_3d ? 1.0 : 0.0);
    if (const XmlNode* p = node.child("Playlist")) {
      st.no_repeat_tracks = int(p->number("noRepeat"));
      for (const auto& t : p->children) {
        if (!iequals(t.name, "MusicTrack")) continue;
        RadioTrack tr;
        tr.title = t.attr("name");
        tr.artist = t.attr("artist");
        tr.name = t.attr("fmodname");
        tr.likelihood_day = std::max(0.0, t.number("likelihoodDaytime", 1.0));
        tr.likelihood_morning = std::max(0.0, t.number("likelihoodMorning", 1.0));
        tr.likelihood_evening = std::max(0.0, t.number("likelihoodEvening", 1.0));
        tr.likelihood_night = std::max(0.0, t.number("likelihoodNight", 1.0));
        tr.clip = resolve(tr.name, music, music_at, out.music);
        if (tr.clip >= 0) st.tracks.push_back(std::move(tr));
      }
    }
    auto list = [&](const char* element, const char* item, std::vector<int>& into, int& no_repeat) {
      const XmlNode* l = node.child(element);
      if (!l) return;
      no_repeat = int(l->number("noRepeat"));
      for (const auto& c : l->children)
        if (iequals(c.name, item))
          if (int k = resolve(c.attr("fmodName"), vo, vo_at, out.vo); k >= 0) into.push_back(k);
    };
    auto special_map = [&](const char* element, std::unordered_map<std::string, int>& into) {
      const XmlNode* l = node.child(element);
      if (!l) return;
      for (const auto& c : l->children) {
        if (!iequals(c.name, "SpecialDialogue")) continue;
        const std::string ev = lower(c.attr("gameplayEvent"));
        const int k = resolve(c.attr("fmodName"), vo, vo_at, out.vo);
        if (!ev.empty() && k >= 0) into[ev] = k;
      }
    };
    list("Idents", "Ident", st.idents, st.no_repeat_idents);
    list("DJRegularDialogue", "SimpleDialogue", st.dj, st.no_repeat_dj);
    list("DJFestivalUpdateLeadIn", "SimpleDialogue", st.festival_lead_in, st.no_repeat_lead_in);
    list("DJFestivalUpdateLeadOut", "SimpleDialogue", st.festival_lead_out, st.no_repeat_lead_out);
    special_map("DJSpecialDialogue", st.special_dialogue);
    special_map("DJImmediateDialogue", st.immediate_dialogue);
    out.stations.push_back(std::move(st));
  }
  int off = -1;
  for (size_t i = 0; i < out.stations.size(); ++i) {
    const auto& st = out.stations[i];
    if (st.off) off = off < 0 ? int(i) : off;
    else if (st.is_3d) {
      if (!st.tracks.empty()) out.location_stations.push_back(int(i));
    } else if (!st.tracks.empty()) {
      out.cycle.push_back(int(i));
    }
  }
  if (out.cycle.empty()) return fail(error, "RadioSystem.xml has no player station");
  if (off >= 0) out.cycle.push_back(off);
  return true;
}

bool parse_radio_vox_pops(std::string_view vox_pops_xml, RadioVoxPops& out, std::string* error) {
  out = RadioVoxPops{};
  XmlNode root;
  if (!parse_xml(vox_pops_xml, root, error)) return false;
  for (const auto& c : root.children) {
    if (iequals(c.name, "Festival")) {
      const std::string ev = c.attr("gameplayEvent");
      if (!ev.empty()) out.events.push_back(ev);
    } else if (iequals(c.name, "Setup")) {
      out.no_repeat = int(c.number("NoRepeat", 0));
      out.interval_min_s = std::max(10.0, c.number("IntervalMin", 3600.0));
      out.interval_max_s = std::max(out.interval_min_s, c.number("IntervalMax", 48000.0));
    }
  }
  return true;
}

bool load_radio(const std::filesystem::path& game_root, RadioData& out, std::string* error,
                std::string_view language) {
  const auto dir = game_root / "media" / "audio" / "radio";
  auto resolve_radio = [&](const std::string& file) { return cars::mods::resolve(dir / file); };
  std::string lang;
  lang.reserve(language.size());
  for (char c : language) lang.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  std::error_code ec;
  if (lang.empty() || !std::filesystem::exists(resolve_radio("Radio_VO_" + lang + ".fsb"), ec) ||
      !std::filesystem::exists(resolve_radio("RadioSoundbankInfo_VO_" + lang + ".xml"), ec))
    lang = "EN";
  const auto system = resolve_radio("RadioSystem.xml");
  const auto music_info = resolve_radio("RadioSoundbankInfo_Music.xml");
  const auto vo_info = resolve_radio("RadioSoundbankInfo_VO_" + lang + ".xml");
  const auto music_bank = resolve_radio("Radio_Music.fsb");
  const auto vo_bank = resolve_radio("Radio_VO_" + lang + ".fsb");
  for (const auto& p : {system, music_info, vo_info, music_bank, vo_bank})
    if (!std::filesystem::exists(p, ec)) return fail(error, "no " + p.string());
  if (!parse_radio(read_text(system), read_text(music_info), read_text(vo_info), out, error)) return false;
  const auto vox_pops = resolve_radio("RadioDJVoxPops.xml");
  if (std::filesystem::exists(vox_pops, ec)) parse_radio_vox_pops(read_text(vox_pops), out.vox_pops, nullptr);
  out.music_bank = music_bank;
  out.vo_bank = vo_bank;
  out.language = lang;
  return true;
}

// ---------------------------------------------------------------- schedule

StationSchedule::StationSchedule(const RadioData& data, int station, uint64_t seed, double t0, bool dj)
    : data_(data), st_(data.stations[size_t(station)]), rng_(seed), use_dj_(dj), station_(uint64_t(station)) {
  tracks_.reset(int(st_.tracks.size()), st_.no_repeat_tracks);
  idents_.reset(int(st_.idents.size()), st_.no_repeat_idents);
  dj_.reset(int(st_.dj.size()), st_.no_repeat_dj);
  lead_in_.reset(int(st_.festival_lead_in.size()), st_.no_repeat_lead_in);
  lead_out_.reset(int(st_.festival_lead_out.size()), st_.no_repeat_lead_out);
  const int t = pick_weighted_track();
  if (t < 0) return;
  const RadioClip& c = data_.music[size_t(st_.tracks[size_t(t)].clip)];
  const double until = c.ident_start > 0 ? c.ident_start : c.length;
  const double u = std::uniform_real_distribution<double>(0, std::max(0.0, until))(rng_);
  events_.push_back({RadioKind::kTrack, st_.tracks[size_t(t)].clip, t, t0 - u, t0 - u + c.length, next_id_++});
}

int StationSchedule::pick_weighted_track() {
  const int n = int(st_.tracks.size());
  if (n <= 0) return -1;
  auto tod_weight = [&](const RadioTrack& tr) {
    const double h = std::fmod(std::fmod(tod_hour_, 24.0) + 24.0, 24.0);
    if (h >= 5.0 && h < 11.0) return tr.likelihood_morning;
    if (h >= 11.0 && h < 17.0) return tr.likelihood_day;
    if (h >= 17.0 && h < 21.0) return tr.likelihood_evening;
    return tr.likelihood_night;
  };
  double total_w = 0.0;
  bool uniform = true;
  double first_w = tod_weight(st_.tracks[0]);
  for (const auto& tr : st_.tracks) {
    const double w = tod_weight(tr);
    total_w += w;
    if (std::fabs(w - first_w) > 1e-6) uniform = false;
  }
  if (uniform || total_w <= 0.0) return tracks_.pick(rng_);
  return tracks_.pick_weighted(rng_, [&](int k) { return tod_weight(st_.tracks[size_t(k)]); });
}

bool StationSchedule::queue_special_dialogue(std::string_view gameplay_event) {
  auto it = st_.special_dialogue.find(lower(gameplay_event));
  if (it == st_.special_dialogue.end()) return false;
  pending_special_.push_back(it->second);
  return true;
}

bool StationSchedule::trigger_immediate_dialogue(std::string_view gameplay_event, double now_clock) {
  auto it = st_.immediate_dialogue.find(lower(gameplay_event));
  if (it == st_.immediate_dialogue.end()) return false;
  const int clip = it->second;
  if (clip < 0 || size_t(clip) >= data_.vo.size()) return false;
  const double len = std::max(0.5, data_.vo[size_t(clip)].length);
  events_.push_back({RadioKind::kDj, clip, -1, now_clock, now_clock + len, next_id_++});
  return true;
}

bool StationSchedule::queue_festival_update(std::string_view gameplay_event) {
  auto it = data_.festival_updates.find(lower(gameplay_event));
  if (it == data_.festival_updates.end()) return false;
  pending_festival_.push_back(it->second);
  return true;
}

void StationSchedule::stop_dj(double now_clock) {
  for (auto it = events_.begin(); it != events_.end();) {
    const bool dj = it->kind == RadioKind::kDj || it->kind == RadioKind::kFestivalUpdate;
    if (dj && it->start > now_clock) {
      it = events_.erase(it);
      continue;
    }
    if (dj && it->end > now_clock) it->end = now_clock;
    ++it;
  }
  pending_special_.clear();
  pending_festival_.clear();
}

void StationSchedule::set_locked_playlist(std::vector<int> track_indices, double now_clock, bool then_normal) {
  locked_tracks_.clear();
  for (int idx : track_indices) {
    if (idx >= 0 && size_t(idx) < st_.tracks.size()) locked_tracks_.push_back(idx);
  }
  locked_cursor_ = 0;
  locked_then_normal_ = then_normal;
  if (locked_tracks_.empty()) return;
  events_.clear();
  const int first_t = locked_tracks_[0];
  locked_cursor_ = 1;
  const RadioClip& c = data_.music[size_t(st_.tracks[size_t(first_t)].clip)];
  events_.push_back({RadioKind::kTrack, st_.tracks[size_t(first_t)].clip, first_t, now_clock, now_clock + c.length,
                     next_id_++});
}

void StationSchedule::clear_locked_playlist() {
  locked_tracks_.clear();
  locked_cursor_ = 0;
}

const RadioEvent* StationSchedule::last_track() const {
  for (auto it = events_.rbegin(); it != events_.rend(); ++it)
    if (it->kind == RadioKind::kTrack) return &*it;
  return nullptr;
}

void StationSchedule::extend() {
  const RadioEvent* last = last_track();
  if (!last) return;
  const RadioClip& lc = data_.music[size_t(last->clip)];
  const double ident_at = last->start + (lc.ident_start > 0 ? lc.ident_start : lc.length);
  double next_at = last->end;
  const int i = idents_.pick(rng_);
  if (i >= 0) {
    const int clip = st_.idents[size_t(i)];
    const RadioClip& ic = data_.vo[size_t(clip)];
    if (!silent_idents_) events_.push_back({RadioKind::kIdent, clip, -1, ident_at, ident_at + ic.length, next_id_++});
    next_at = ident_at + (ic.next_track > 0 ? ic.next_track : ic.length);
  }
  int t = -1;
  if (!locked_tracks_.empty() && locked_then_normal_ && locked_cursor_ >= locked_tracks_.size()) {
    // The list has played: the station's own picks from here.
    locked_tracks_.clear();
    locked_cursor_ = 0;
  }
  if (!locked_tracks_.empty()) {
    t = locked_tracks_[locked_cursor_ % locked_tracks_.size()];
    ++locked_cursor_;
  } else {
    t = pick_weighted_track();
  }
  if (t < 0) return;
  const RadioClip& tc = data_.music[size_t(st_.tracks[size_t(t)].clip)];
  events_.push_back({RadioKind::kTrack, st_.tracks[size_t(t)].clip, t, next_at, next_at + tc.length, next_id_++});

  // Priority 1: Festival Update (LeadIn -> Update -> LeadOut).
  if (!pending_festival_.empty()) {
    const int fu_clip = pending_festival_.front();
    pending_festival_.pop_front();
    double cursor = next_at;
    if (const int li = lead_in_.pick(rng_); li >= 0) {
      const int clip = st_.festival_lead_in[size_t(li)];
      const double len = data_.vo[size_t(clip)].length;
      events_.push_back({RadioKind::kFestivalUpdate, clip, -1, cursor, cursor + len, next_id_++});
      cursor += len;
    }
    if (fu_clip >= 0 && size_t(fu_clip) < data_.vo.size()) {
      const double len = data_.vo[size_t(fu_clip)].length;
      events_.push_back({RadioKind::kFestivalUpdate, fu_clip, -1, cursor, cursor + len, next_id_++});
      cursor += len;
    }
    if (const int lo = lead_out_.pick(rng_); lo >= 0) {
      const int clip = st_.festival_lead_out[size_t(lo)];
      const double len = data_.vo[size_t(clip)].length;
      events_.push_back({RadioKind::kFestivalUpdate, clip, -1, cursor, cursor + len, next_id_++});
    }
    tracks_since_dj_ = 0;
    return;
  }

  // Priority 2: Special Dialogue (barn-find / location hints).
  if (!pending_special_.empty()) {
    const int sp_clip = pending_special_.front();
    pending_special_.pop_front();
    if (sp_clip >= 0 && size_t(sp_clip) < data_.vo.size()) {
      events_.push_back({RadioKind::kDj, sp_clip, -1, next_at, next_at + data_.vo[size_t(sp_clip)].length, next_id_++});
      tracks_since_dj_ = 0;
      return;
    }
  }

  // Priority 3: Regular DJ banter respecting maxTracksBetweenDJ.
  ++tracks_since_dj_;
  if (use_dj_ && tracks_since_dj_ >= std::max(1, data_.levels.max_tracks_between_dj)) {
    const int d = dj_.pick(rng_, [&](int k) { return data_.vo[size_t(st_.dj[size_t(k)])].length <= tc.event_start; });
    if (d >= 0) {
      const int clip = st_.dj[size_t(d)];
      events_.push_back({RadioKind::kDj, clip, -1, next_at, next_at + data_.vo[size_t(clip)].length, next_id_++});
      tracks_since_dj_ = 0;
    }
  }
}

void StationSchedule::ensure_extended(double t0, double t1) {
  for (const RadioEvent* last = last_track(); last && last->start <= t1; last = last_track()) {
    const size_t before = events_.size();
    extend();
    if (events_.size() == before) break;
  }
  const RadioEvent* keep = last_track();
  const uint64_t keep_id = keep ? keep->id : ~0ull;
  std::erase_if(events_, [&](const RadioEvent& e) { return e.end <= t0 && e.id != keep_id; });
}

void StationSchedule::audible(double t0, double t1, std::vector<RadioEvent>& out) {
  out.clear();
  ensure_extended(t0, t1);
  for (const auto& e : events_)
    if (e.start < t1 && e.end > t0) {
      out.push_back(e);
      out.back().id = e.id | station_ << 48;
    }
}

const RadioEvent* StationSchedule::track_at(double t, double* offset) {
  ensure_extended(t, t);
  const RadioEvent* best = nullptr;
  for (const auto& e : events_)
    if (e.kind == RadioKind::kTrack && e.start <= t && (!best || e.start > best->start)) best = &e;
  if (best && offset) *offset = t - best->start;
  return best;
}

// ------------------------------------------------------------------- radio

Radio::Radio(const RadioData& data, uint64_t seed, bool dj) : data_(data), dj_enabled_(dj) {
  schedules_.resize(data.stations.size());
  for (size_t i = 0; i < data.cycle.size(); ++i) {
    const int s = data.cycle[i];
    if (!data.stations[size_t(s)].off)
      schedules_[size_t(s)] = std::make_unique<StationSchedule>(data, s, seed * 0x9E3779B97F4A7C15ull + i + 1, 0.0, dj);
  }
  for (size_t i = 0; i < data.location_stations.size(); ++i) {
    const int s = data.location_stations[i];
    if (!schedules_[size_t(s)])
      schedules_[size_t(s)] =
          std::make_unique<StationSchedule>(data, s, seed * 0xBF58476D1CE4E5B9ull + i + 17, 0.0, false);
  }
  next_vox_pop_s_ = std::max(60.0, data.vox_pops.interval_min_s);
}

bool Radio::on() const {
  const int s = selected();
  return s >= 0 && !data_.stations[size_t(s)].off;
}

StationSchedule* Radio::schedule(int station) {
  return station >= 0 && size_t(station) < schedules_.size() ? schedules_[size_t(station)].get() : nullptr;
}

void Radio::select(int station, bool user_change) {
  for (size_t i = 0; i < data_.cycle.size(); ++i)
    if (data_.cycle[i] == station) pos_ = int(i);
  last_change_was_user_ = user_change;
  seen_track_ = ~0ull;
  if (auto* s = schedule(selected()))
    if (const RadioEvent* t = s->track_at(clock_)) seen_track_ = t->id;
}

int Radio::find_station(std::string_view name) const {
  for (size_t i = 0; i < data_.stations.size(); ++i)
    if (iequals(data_.stations[i].name, name)) return int(i);
  return -1;
}

std::string Radio::selected_name() const {
  const int s = selected();
  return s >= 0 && size_t(s) < data_.stations.size() ? data_.stations[size_t(s)].name : std::string{};
}

void Radio::set_dj_enabled(bool on) {
  dj_enabled_ = on;
  for (auto& s : schedules_)
    if (s) s->set_dj_enabled(on);
}

void Radio::set_time_of_day_hour(double hour) {
  for (auto& s : schedules_)
    if (s) s->set_time_of_day_hour(hour);
}

bool Radio::trigger_special_dialogue(std::string_view gameplay_event) {
  bool any = false;
  for (int s : data_.cycle)
    if (auto* sc = schedule(s)) any |= sc->queue_special_dialogue(gameplay_event);
  return any;
}

bool Radio::trigger_immediate_dialogue(std::string_view gameplay_event) {
  if (auto* sc = schedule(selected()); sc && on()) {
    return sc->trigger_immediate_dialogue(gameplay_event, clock_);
  }
  return false;
}

bool Radio::trigger_festival_update(std::string_view gameplay_event) {
  bool any = false;
  for (int s : data_.cycle)
    if (auto* sc = schedule(s)) any |= sc->queue_festival_update(gameplay_event);
  if (any) since_festival_update_s_ = 0.0;
  return any;
}

void Radio::cancel_festival_updates() {
  for (int s : data_.cycle)
    if (auto* sc = schedule(s)) sc->cancel_festival_updates();
}

bool Radio::lock_viper_intro_playlist() {
  // Locate Bass Arena (Radio1 / station containing "Bass", or first music station with Levels/Language).
  int target_station = -1;
  for (int s : data_.cycle) {
    if (s < 0 || size_t(s) >= data_.stations.size() || data_.stations[size_t(s)].off) continue;
    const std::string nm = lower(data_.stations[size_t(s)].name);
    if (nm.find("bass") != std::string::npos || nm == "radio1") {
      target_station = s;
      break;
    }
  }
  if (target_station < 0) {
    for (int s : data_.cycle) {
      if (s >= 0 && size_t(s) < data_.stations.size() && !data_.stations[size_t(s)].off &&
          !data_.stations[size_t(s)].tracks.empty()) {
        target_station = s;
        break;
      }
    }
  }
  if (target_station < 0) return false;
  const auto& st = data_.stations[size_t(target_station)];
  if (st.tracks.empty()) return false;

  int idx_levels = -1, idx_language = -1;
  for (size_t i = 0; i < st.tracks.size(); ++i) {
    const std::string tlow = lower(st.tracks[i].title + " " + st.tracks[i].name);
    if (idx_levels < 0 && tlow.find("levels") != std::string::npos) idx_levels = int(i);
    if (idx_language < 0 && tlow.find("language") != std::string::npos) idx_language = int(i);
  }
  std::vector<int> locked;
  if (idx_levels >= 0) locked.push_back(idx_levels);
  if (idx_language >= 0 && idx_language != idx_levels) locked.push_back(idx_language);
  if (locked.empty()) {
    locked.push_back(0);
    if (st.tracks.size() > 1) locked.push_back(1);
  } else if (locked.size() == 1 && st.tracks.size() > 1) {
    locked.push_back(locked[0] == 0 ? 1 : 0);
  }

  return lock_viper_intro_playlist(target_station, std::move(locked));
}

bool Radio::lock_viper_intro_playlist(int station, std::vector<int> track_indices) {
  if (station < 0 || size_t(station) >= data_.stations.size()) return false;
  const auto& st = data_.stations[size_t(station)];
  if (st.off || st.tracks.empty() || track_indices.empty()) return false;
  std::vector<int> valid;
  for (int ti : track_indices) {
    if (ti >= 0 && size_t(ti) < st.tracks.size()) valid.push_back(ti);
  }
  if (valid.empty()) return false;
  select(station);
  if (auto* sc = schedule(station)) {
    sc->set_locked_playlist(std::move(valid), clock_);
    if (const RadioEvent* t = sc->track_at(clock_)) seen_track_ = t->id;
  }
  viper_intro_locked_ = true;
  return true;
}

void Radio::unlock_viper_intro_playlist() {
  viper_intro_locked_ = false;
  for (auto& sc : schedules_) {
    if (sc) sc->clear_locked_playlist();
  }
}

bool Radio::set_playlist(const std::vector<std::string>& fmodnames, bool then_normal) {
  if (fmodnames.empty()) return false;
  // The station that carries the first name; the rest must be its tracks too.
  for (int s : data_.cycle) {
    if (s < 0 || size_t(s) >= data_.stations.size() || data_.stations[size_t(s)].off) continue;
    const auto& st = data_.stations[size_t(s)];
    std::vector<int> idx;
    for (const std::string& want : fmodnames) {
      for (size_t i = 0; i < st.tracks.size(); ++i)
        if (iequals(st.tracks[i].name, want)) {
          idx.push_back(int(i));
          break;
        }
    }
    if (idx.empty()) continue;
    select(s);
    if (auto* sc = schedule(s)) {
      sc->set_locked_playlist(std::move(idx), clock_, then_normal);
      if (const RadioEvent* t = sc->track_at(clock_)) seen_track_ = t->id;
    }
    // The list's first track is a new track: its popup after the delay, or
    // once the HUD is back (Everyday at the original's hand-over, The Power
    // at the race to the festival; Q273).
    if (popup_held_) {
      popup_deferred_ = true;
    } else {
      popup_from_ = popup_clock_ + data_.levels.new_track_delay;
      popup_station_ = s;
      popup_press_ = false;
    }
    return true;
  }
  return false;
}

void Radio::cancel_playlist() {
  for (auto& sc : schedules_)
    if (sc) sc->clear_locked_playlist();
}

void Radio::set_silent_idents(bool on) {
  silent_idents_ = on;
  for (auto& sc : schedules_)
    if (sc) sc->set_silent_idents(on);
}

void Radio::stop_dj() {
  for (auto& sc : schedules_)
    if (sc) sc->stop_dj(clock_);
}

void Radio::press(int dir) {
  if (viper_intro_locked_ || !switching_enabled_) return;
  const int n = int(data_.cycle.size());
  if (n == 0) return;
  select(data_.cycle[size_t(((pos_ + dir) % n + n) % n)]);
  last_change_was_user_ = true;
  popup_station_ = selected();
  popup_from_ = popup_clock_ + (on() ? kPopupDelay : 0.0);
  popup_press_ = true;
}

void Radio::advance(double dt, bool run) {
  popup_clock_ += dt;
  if (!on()) {
    if (run) clock_ += dt;
    return;
  }
  clock_ += dt;
  since_festival_update_s_ += dt;
  if (dj_enabled_ && !data_.vox_pops.events.empty() && since_festival_update_s_ >= next_vox_pop_s_) {
    const std::string& ev = data_.vox_pops.events[vox_pop_rr_++ % data_.vox_pops.events.size()];
    trigger_festival_update(ev);
    since_festival_update_s_ = 0.0;
    next_vox_pop_s_ = std::max(60.0, data_.vox_pops.interval_min_s);
  }
  // A new track on the selected station: its popup after the delay.
  if (const RadioEvent* t = schedule(selected())->track_at(clock_); t && t->id != seen_track_) {
    seen_track_ = t->id;
    const double from = popup_clock_ + data_.levels.new_track_delay;
    // Not over a popup showing or due (a press's).
    if (popup_held_) {
      popup_deferred_ = true;
    } else if (popup_clock_ - popup_from_ > kPopupShow) {
      popup_from_ = from;
      popup_station_ = selected();
      popup_press_ = false;
    }
  }
}

void Radio::hold_popup(bool held) {
  if (held == popup_held_) return;
  popup_held_ = held;
  if (held) {
    // A popup showing or due goes with the HUD and comes back with it.
    popup_deferred_ = popup_station_ >= 0 && popup_clock_ - popup_from_ <= kPopupShow;
    popup_from_ = -1e9;
  } else if (popup_deferred_) {
    popup_deferred_ = false;
    popup_from_ = popup_clock_ + data_.levels.new_track_delay;
    popup_station_ = selected();
    popup_press_ = false;
  }
}

Radio::Popup Radio::popup() const {
  Popup p;
  const double age = popup_clock_ - popup_from_;
  if (popup_held_ || age < 0 || age > kPopupShow || popup_station_ < 0) return p;
  p.alpha = std::clamp(std::min(age, kPopupShow - age) / kPopupFade, 0.0, 1.0);
  p.station = popup_station_;
  p.press = popup_press_;
  if (popup_station_ == selected() && on())
    if (auto* s = const_cast<Radio*>(this)->schedule(popup_station_))
      if (const RadioEvent* t = s->track_at(clock_)) p.track = t->track;
  return p;
}

// ------------------------------------------------------------------ player

struct RadioPlayer::Banks {
  std::vector<FsbSample> music, vo;
  std::filesystem::path music_bank, vo_bank;
  std::unique_ptr<StreamPool> pool;
  ~Banks() { pool.reset(); }  // stop worker before vectors/paths destroy
};

namespace {
bool read_bank_headers(const std::filesystem::path& p, std::vector<FsbSample>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  uint8_t head[48];
  if (!f.read(reinterpret_cast<char*>(head), 48)) return false;
  const uint32_t header_bytes = uint32_t(head[8]) | uint32_t(head[9]) << 8 | uint32_t(head[10]) << 16 |
                                uint32_t(head[11]) << 24;
  std::vector<uint8_t> bytes(48 + size_t(header_bytes));
  std::memcpy(bytes.data(), head, 48);
  if (!f.read(reinterpret_cast<char*>(bytes.data() + 48), header_bytes)) return false;
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(p, ec);
  return !ec && read_fsb4(bytes, out, nullptr, size);
}
}  // namespace

RadioPlayer::RadioPlayer(Radio& radio, int out_rate, bool non_blocking)
    : banks_(std::make_unique<Banks>()), radio_(radio), rate_(std::max(1, out_rate)), non_blocking_(non_blocking) {
  banks_->music_bank = radio.data().music_bank;
  banks_->vo_bank = radio.data().vo_bank;
  read_bank_headers(banks_->music_bank, banks_->music);
  read_bank_headers(banks_->vo_bank, banks_->vo);
  Banks* b = banks_.get();
  banks_->pool = std::make_unique<StreamPool>(16, 98304, [b](uint64_t key) -> std::unique_ptr<PcmSource> {
    const bool is_music = (key >> 32) != 0;
    const uint32_t index = uint32_t(key & 0xFFFFFFFFu);
    const auto& samples = is_music ? b->music : b->vo;
    if (index >= samples.size()) return nullptr;
    const FsbSample& s = samples[index];
    return make_fsb_mpeg_source(is_music ? b->music_bank : b->vo_bank, s.offset, s.bytes,
                                s.rate > 0 ? s.rate : 48000);
  });
  station_gain_.assign(radio.data().stations.size(), 0.0);
  if (radio.on()) {
    station_gain_[size_t(radio.selected())] = 1;
    last_selected_ = radio.selected();
  }
  // Pre-allocate audio-thread scratch buffers so steady-state rendering never allocates.
  scratch_.reserve(32);
  mix_.resize(6 * 4096, 0.0f);
  loc_mono_.resize(4096, 0.0f);
  homespace_stations_[0] = radio.find_station("Radio6_3d_Autoshow");
  homespace_stations_[1] = radio.find_station("Radio8_3d_Paintshop");
  homespace_stations_[2] = radio.find_station("Radio7_3d_Workshop");
}

RadioPlayer::~RadioPlayer() {
  for (auto& v : voices_) {
    if (v.active) release_voice(v);
  }
}

int RadioPlayer::decoders() const {
  int n = 0;
  for (const auto& v : voices_)
    if (v.active) ++n;
  return n;
}

void RadioPlayer::release_voice(Voice& v) {
  if (v.slot >= 0 && banks_ && banks_->pool) banks_->pool->release(v.slot);
  v = Voice{};
}

RadioPlayer::Voice* RadioPlayer::voice_for(uint64_t voice_id, const RadioEvent& e, int64_t initial_frame) {
  for (auto& v : voices_)
    if (v.active && v.id == voice_id) return &v;
  Voice* free_slot = nullptr;
  for (auto& v : voices_) {
    if (!v.active) {
      free_slot = &v;
      break;
    }
  }
  if (!free_slot) {
    for (auto& v : voices_) {
      if (!v.used) {
        release_voice(v);
        free_slot = &v;
        break;
      }
    }
  }
  if (!free_slot) return nullptr;
  const bool music = e.kind == RadioKind::kTrack;
  const auto& clips = music ? radio_.data().music : radio_.data().vo;
  int slot = -1;
  if (e.clip >= 0 && size_t(e.clip) < clips.size()) {
    const uint32_t sample = clips[size_t(e.clip)].sample;
    const uint64_t key = (music ? (1ull << 32) : 0ull) | uint64_t(sample);
    slot = banks_->pool->acquire(key, std::max<int64_t>(0, initial_frame));
  }
  *free_slot = Voice{};
  free_slot->active = true;
  free_slot->id = voice_id;
  free_slot->slot = slot;
  free_slot->used = true;
  free_slot->pos = double(std::max<int64_t>(0, initial_frame));
  return free_slot;
}

void RadioPlayer::render(float* out, int frames) {
  if (frames <= 0) return;
  const RadioData& data = radio_.data();
  const RadioLevels& lv = data.levels;
  const double dt = double(frames) / rate_;
  const int sel = radio_.on() ? radio_.selected() : -1;
  const bool user_change = radio_.last_change_was_user();
  if (sel != last_selected_) {
    if (user_change && last_selected_ >= 0 && sel >= 0) {
      switch_gap_rem_s_ = kStationSwitchGapS;
    } else if (!user_change) {
      switch_gap_rem_s_ = 0.0;
    }
    last_selected_ = sel;
  }
  bool fading = false;
  for (size_t s = 0; s < station_gain_.size(); ++s) fading |= station_gain_[s] > 0 && int(s) != sel;
  int gap_frames = 0;
  if (user_change && !fading && switch_gap_rem_s_ > 0.0) {
    gap_frames = std::min(frames, int(std::llround(switch_gap_rem_s_ * rate_)));
    switch_gap_rem_s_ = std::max(0.0, switch_gap_rem_s_ - dt);
  }
  const double t0 = radio_.clock();
  // The clock runs while on, and while the last station fades to off.
  const double t1 = t0 + (sel >= 0 || fading ? dt : 0);
  for (auto& v : voices_) {
    if (v.active && !v.location && !v.homespace) v.used = false;
  }
  const size_t mix_need = 6 * size_t(frames);
  if (mix_.size() < mix_need) mix_.resize(mix_need, 0.0f);
  float* music = mix_.data();
  float* voice = mix_.data() + 2 * size_t(frames);
  float* ident = mix_.data() + 4 * size_t(frames);
  const double fade_s = user_change ? lv.change_fade : lv.change_fade_normal;
  const double fade_step = 1.0 / (std::max(0.01, fade_s) * rate_);
  // A D-pad change fades the old station out, holds ~0.25 s of silence, and
  // brings the new one in over 50 ms (Q335: the original dips to silence
  // between stations; it does not crossfade). A change by the game keeps the
  // crossfade over stationChangeFadeTimeNormal.
  constexpr double kStationFadeInS = 0.05;
  const double fade_in_step = user_change ? 1.0 / (kStationFadeInS * rate_) : fade_step;
  const double vol = std::clamp(volume, 0.0, 2.0);
  bool any_sel_dj = false;
  bool any_sel_festival = false;
  for (size_t s = 0; s < station_gain_.size(); ++s) {
    const bool hold_for_switch = user_change && (fading || gap_frames >= frames);
    const double target = int(s) == sel && !hold_for_switch ? 1.0 : 0.0;
    if (station_gain_[s] <= 0 && target <= 0) {
      // Warm up the incoming station's decoder during the fade-out and silence hold
      // so audio is ready immediately when the 50 ms fade-in begins.
      if (int(s) == sel && hold_for_switch && t1 > t0) {
        if (StationSchedule* sched = radio_.schedule(int(s))) {
          sched->audible(t0, t1 + prefetch_s_, scratch_);
          for (const auto& e : scratch_) {
            const bool now_audible = e.start < t1 && e.end > t0;
            const double approx_start = now_audible ? std::max(0.0, (t0 - e.start) * 48000.0) : 0.0;
            if (Voice* v = voice_for(e, int64_t(approx_start))) v->used = true;
          }
        }
      }
      continue;
    }
    StationSchedule* sched = radio_.schedule(int(s));
    if (!sched || t1 <= t0) {
      station_gain_[s] = target;
      continue;
    }
    std::fill_n(mix_.data(), mix_need, 0.0f);
    // Ask ahead by prefetch_s_ so upcoming idents, tracks and DJ lines are
    // already decoded on the worker thread before they start.
    sched->audible(t0, t1 + (int(s) == sel ? prefetch_s_ : 0.0), scratch_);
    bool dj = false;
    bool fest = false;
    for (const auto& e : scratch_) {
      const bool now_audible = e.start < t1 && e.end > t0;
      const int first = now_audible ? std::clamp(int(std::ceil((e.start - t0) * rate_)), 0, frames) : 0;
      const int last = now_audible ? std::clamp(int(std::ceil((e.end - t0) * rate_)), 0, frames) : 0;
      const double approx_start = now_audible ? std::max(0.0, (t0 + double(first) / rate_ - e.start) * 48000.0) : 0.0;
      Voice* v = voice_for(e, int64_t(approx_start));
      if (!v) continue;
      v->used = true;
      if (!now_audible || last <= first || v->slot < 0) continue;
      dj |= (e.kind == RadioKind::kDj || e.kind == RadioKind::kFestivalUpdate);
      fest |= (e.kind == RadioKind::kFestivalUpdate);
      PcmStream* st = banks_->pool->stream(v->slot);
      if (!st) continue;
      if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned()))
        banks_->pool->wait_ready(v->slot);
      if (st->state() != PcmStream::kReady || !st->positioned()) continue;
      const int src_rate = st->rate();
      const double step = double(src_rate) / rate_;
      const double expect = std::max(0.0, (t0 + double(first) / rate_ - e.start) * src_rate);
      const double drift = expect - v->pos;
      double step_eff = step;
      if (std::fabs(drift) > 0.25 * src_rate) {
        // Large seek/station-switch jump: reposition cleanly and ramp up smoothly (zero click).
        v->pos = expect;
        v->env = 0.0f;
        const int64_t need = int64_t(std::floor(v->pos));
        if (need < st->from() || need > st->end() + 4096) {
          st->request(need);
          if (!non_blocking_) banks_->pool->wait_ready(v->slot);
          if (!st->positioned()) continue;
        }
      } else if (std::fabs(drift) > 0.004 * src_rate) {
        // Gentle PLL pull eliminates micro-drift without cutting audio or clicking.
        const double pull = std::clamp(1.0 + drift / (0.35 * double(src_rate)), 0.985, 1.015);
        step_eff = step * pull;
      }
      const int n = last - first;
      if (!non_blocking_) {
        const int64_t want_end = int64_t(v->pos + step_eff * n) + 2;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!st->source_done() && st->end() < want_end && std::chrono::steady_clock::now() < deadline) {
          st->hint(std::max<int64_t>(st->from(), int64_t(std::floor(v->pos)) - 4));
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      const double gain = lv.master * (e.kind == RadioKind::kTrack   ? lv.music
                                       : e.kind == RadioKind::kIdent ? lv.ident
                                                                     : lv.dialogue);
      float* dst = (e.kind == RadioKind::kTrack   ? music
                    : e.kind == RadioKind::kIdent ? ident
                                                  : voice) +
                   2 * size_t(first);
      const int64_t rfrom = st->from(), rend = st->end();
      const float env_up = 1.0f / std::max(1.0f, 0.008f * float(rate_));
      const float env_dn = 1.0f / std::max(1.0f, 0.004f * float(rate_));
      const float decay = 0.985f;
      const double inv_rate = 1.0 / double(rate_);
      constexpr double kEdgeFadeS = 0.008;
      bool missed = false;
      for (int i = 0; i < n; ++i, v->pos += step_eff) {
        const int64_t a = int64_t(std::floor(v->pos));
        if (a < rfrom || a >= rend) {
          missed = true;
          v->env = std::max(0.0f, v->env - env_dn);
          v->last_l *= decay;
          v->last_r *= decay;
          if (v->env > 1e-5f) {
            dst[2 * i] += float(gain) * v->last_l * v->env;
            dst[2 * i + 1] += float(gain) * v->last_r * v->env;
          }
          continue;
        }
        const double frac = v->pos - double(a);
        float al = 0, ar = 0, bl = 0, br = 0;
        st->sample(a, al, ar);
        if (a + 1 < rend) {
          st->sample(a + 1, bl, br);
          al = float(al + (bl - al) * frac);
          ar = float(ar + (br - ar) * frac);
        }
        if (!std::isfinite(al)) al = 0.0f;
        if (!std::isfinite(ar)) ar = 0.0f;
        v->env = std::min(1.0f, v->env + env_up);
        const double rem_s = e.end - (t0 + double(first + i) * inv_rate);
        const float edge_env = (rem_s < kEdgeFadeS) ? float(std::clamp(rem_s / kEdgeFadeS, 0.0, 1.0)) : 1.0f;
        const float eff_env = v->env * edge_env;
        v->last_l = al;
        v->last_r = ar;
        dst[2 * i] += float(gain) * al * eff_env;
        dst[2 * i + 1] += float(gain) * ar * eff_env;
      }
      const int64_t cur = int64_t(std::floor(v->pos));
      if (missed && cur < rfrom) st->request(cur);
      else st->hint(std::max<int64_t>(rfrom, cur - 4));
    }
    const double duck_target = dj ? lv.duck : 1.0;
    if (int(s) == sel && dj) any_sel_dj = true;
    if (int(s) == sel && fest) any_sel_festival = true;
    const double duck_step = (1 - lv.duck) / (std::max(0.01, duck_target < duck_ ? lv.duck_down : lv.duck_up) * rate_);
    const double st_vol = s < data.stations.size() ? std::clamp(data.stations[s].volume, 0.0, 2.0) : 1.0;
    double g = station_gain_[s];
    for (int i = 0; i < frames; ++i) {
      const double eff_target = (int(s) == sel && i < gap_frames) ? 0.0 : target;
      g = eff_target > g ? std::min(eff_target, g + fade_in_step) : std::max(eff_target, g - fade_step);
      if (eff_target == 0.0 && g < 1e-9) g = 0.0;
      if (int(s) == sel) duck_ = duck_target < duck_ ? std::max(duck_target, duck_ - duck_step)
                                                     : std::min(duck_target, duck_ + duck_step);
      const double duck = int(s) == sel ? duck_ : 1.0;
      const float k = float(g * master * vol * st_vol);
      out[2 * i] += k * float(music[2 * i] * duck + ident[2 * i] + voice[2 * i] * dj_modifier_);
      out[2 * i + 1] += k * float(music[2 * i + 1] * duck + ident[2 * i + 1] + voice[2 * i + 1] * dj_modifier_);
    }
    station_gain_[s] = g;
  }
  for (auto& v : voices_) {
    if (v.active && !v.location && !v.homespace && !v.used) {
      release_voice(v);
    }
  }
  dj_speaking_ = any_sel_dj;
  festival_update_playing_ = any_sel_festival;
  // The clock runs while on, and while the last station fades to off.
  radio_.advance(dt, fading);
}

std::vector<LocationRadioEmitter> default_colorado_location_emitters(const Radio& radio) {
  std::vector<LocationRadioEmitter> out;
  const int pre_race = radio.find_station("Radio5_3d_PreRace");
  const int autoshow = radio.find_station("Radio6_3d_Autoshow");
  const int workshop = radio.find_station("Radio7_3d_Workshop");
  const int paintshop = radio.find_station("Radio8_3d_Paintshop");
  int streetrace = radio.find_station("Radio9_3d_Streetrace");
  if (streetrace < 0) streetrace = pre_race;
  const int outpost_st = pre_race >= 0 ? pre_race : autoshow;

  auto push = [&](int st, std::array<float, 3> pos, float min_d, float max_d) {
    if (st >= 0) out.push_back({st, pos, min_d, max_d});
  };

  // Horizon Festival main stages & hub areas.
  push(pre_race, {-1468.0f, 26.0f, -345.0f}, 18.0f, 280.0f);
  push(pre_race, {-1398.0f, 1.0f, -22.0f}, 16.0f, 240.0f);
  push(autoshow, {-1020.0f, 0.0f, -260.0f}, 12.0f, 160.0f);
  push(workshop, {-960.0f, 0.0f, -180.0f}, 12.0f, 160.0f);
  push(paintshop, {-1080.0f, 0.0f, -190.0f}, 12.0f, 160.0f);

  // Illegal Street Race Hubs (Carson, Gladstone, Red Rock).
  push(streetrace, {-2100.0f, 12.0f, 1450.0f}, 14.0f, 190.0f);
  push(streetrace, {900.0f, 18.0f, 2100.0f}, 14.0f, 190.0f);
  push(streetrace, {-3200.0f, 35.0f, -2400.0f}, 14.0f, 190.0f);

  // Horizon Outposts (Beaumont, Clear Springs, Finley Dam, Bunker, Montano Plains, Golf Course).
  push(outpost_st, {-1120.0f, 25.0f, 2320.0f}, 12.0f, 145.0f);
  push(outpost_st, {1640.0f, 30.0f, 680.0f}, 12.0f, 145.0f);
  push(outpost_st, {2450.0f, 40.0f, -1520.0f}, 12.0f, 145.0f);
  push(outpost_st, {-420.0f, 28.0f, -2680.0f}, 12.0f, 145.0f);
  push(outpost_st, {1850.0f, 18.0f, -3100.0f}, 12.0f, 145.0f);
  push(outpost_st, {-2650.0f, 22.0f, -620.0f}, 12.0f, 145.0f);
  return out;
}

void RadioPlayer::set_location_emitters(std::vector<LocationRadioEmitter> emitters) {
  emitters_ = std::move(emitters);
  emitter_gain_.assign(emitters_.size(), 0.0f);
  emitter_filters_.assign(emitters_.size(), EmitterFilter{});
}

void RadioPlayer::add_location_emitter(LocationRadioEmitter emitter) {
  emitters_.push_back(emitter);
  emitter_gain_.push_back(0.0f);
  emitter_filters_.push_back(EmitterFilter{});
}

void RadioPlayer::set_listener(std::array<float, 3> pos, float heading_rad) {
  listener_pos_ = pos;
  listener_heading_ = heading_rad;
}

void RadioPlayer::render_location_stations(float* out, int frames) {
  if (frames <= 0) return;
  const double dt = double(frames) / double(rate_);
  const double t0 = radio_.location_clock();
  const double t1 = t0 + dt;
  // Decoupled from player radio playback: advance the 3D location clock
  // unconditionally so UI music, custom radio, or Radio Off never freeze PA speakers.
  radio_.advance_location_clock(dt);
  if (!out || emitters_.empty()) return;
  if (emitter_gain_.size() != emitters_.size()) emitter_gain_.assign(emitters_.size(), 0.0f);
  if (emitter_filters_.size() != emitters_.size()) emitter_filters_.assign(emitters_.size(), EmitterFilter{});
  if (loc_mono_.size() < size_t(frames)) loc_mono_.resize(size_t(frames), 0.0f);

  for (auto& v : voices_) {
    if (v.active && v.location) v.used = false;
  }

  const RadioData& data = radio_.data();
  const float rx = std::cos(listener_heading_);
  const float rz = -std::sin(listener_heading_);
  const float gain_sm = 1.0f - std::exp(-1.0f / (0.045f * float(rate_)));
  const float env_up = 1.0f / std::max(1.0f, 0.008f * float(rate_));
  const float env_dn = 1.0f / std::max(1.0f, 0.006f * float(rate_));
  // External PA high-pass (~250 Hz) removes sub-bass boom so stage bleed never clashes with in-car radio.
  constexpr float kTwoPi = 6.28318530718f;
  const float hp_rc = 1.0f / (kTwoPi * 250.0f);
  const float hp_alpha = hp_rc / (hp_rc + 1.0f / float(rate_));

  // Collect up to 32 audible emitters and deduplicate their stations so each
  // active location station is decoded & advanced ONCE per block (preventing
  // 2x double-speed stuttering when overlapping Festival/Outpost speakers share a station).
  struct AudibleEmitter {
    size_t idx = 0;
    int station = -1;
    float target_att2 = 0.0f;
    float dist_norm = 0.0f;
    float pan_l = 0.70710678f;
    float pan_r = 0.70710678f;
  };
  AudibleEmitter audible[32];
  int audible_count = 0;
  int unique_stations[16];
  int unique_count = 0;

  for (size_t ei = 0; ei < emitters_.size(); ++ei) {
    const auto& em = emitters_[ei];
    if (em.station < 0 || size_t(em.station) >= data.stations.size()) continue;
    const float dx = em.pos[0] - listener_pos_[0];
    const float dy = em.pos[1] - listener_pos_[1];
    const float dz = em.pos[2] - listener_pos_[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float span = std::max(1.0f, em.max_distance - em.min_distance);
    const float dist_norm =
        dist <= em.min_distance ? 0.0f : (dist >= em.max_distance ? 1.0f : std::clamp((dist - em.min_distance) / span, 0.0f, 1.0f));
    const float att = 1.0f - dist_norm;
    const float target_att2 = att * att;
    if (target_att2 <= 1e-5f && emitter_gain_[ei] <= 1e-5f) {
      emitter_gain_[ei] = 0.0f;
      emitter_filters_[ei] = EmitterFilter{};
      continue;
    }
    if (audible_count < 32) {
      const auto& st_def = data.stations[size_t(em.station)];
      const float pan_raw = dist > 1e-3f ? std::clamp((dx * rx + dz * rz) / dist, -1.0f, 1.0f) : 0.0f;
      const float pan = pan_raw * float(std::clamp(st_def.pan_level_3d, 0.0, 1.0));
      AudibleEmitter ae{};
      ae.idx = ei;
      ae.station = em.station;
      ae.target_att2 = target_att2;
      ae.dist_norm = dist_norm;
      ae.pan_l = std::sqrt(0.5f * (1.0f - pan));
      ae.pan_r = std::sqrt(0.5f * (1.0f + pan));
      audible[audible_count++] = ae;

      bool seen = false;
      for (int u = 0; u < unique_count; ++u) {
        if (unique_stations[u] == em.station) {
          seen = true;
          break;
        }
      }
      if (!seen && unique_count < 16) unique_stations[unique_count++] = em.station;
    }
  }

  for (int u = 0; u < unique_count; ++u) {
    const int st_idx = unique_stations[u];
    StationSchedule* sched = radio_.schedule(st_idx);
    if (!sched) continue;
    const auto& st_def = data.stations[size_t(st_idx)];
    const float base_gain = float(st_def.volume * master * volume);
    std::fill_n(loc_mono_.data(), size_t(frames), 0.0f);
    sched->audible(t0, t1 + 1.0, scratch_);
    bool decoded_any = false;
    for (const auto& e : scratch_) {
      const bool now_audible = e.start < t1 && e.end > t0;
      const int first = now_audible ? std::clamp(int(std::ceil((e.start - t0) * rate_)), 0, frames) : 0;
      const int last = now_audible ? std::clamp(int(std::ceil((e.end - t0) * rate_)), 0, frames) : 0;
      const double approx_start = now_audible ? std::max(0.0, (t0 + double(first) / rate_ - e.start) * 48000.0) : 0.0;
      Voice* v = voice_for(e, int64_t(approx_start));
      if (!v) continue;
      v->location = true;
      v->used = true;
      if (!now_audible || last <= first || v->slot < 0) continue;
      PcmStream* st = banks_->pool->stream(v->slot);
      if (!st) continue;
      if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned()))
        banks_->pool->wait_ready(v->slot);
      if (st->state() != PcmStream::kReady || !st->positioned()) continue;
      const int src_rate = st->rate();
      const double step = double(src_rate) / double(rate_);
      const double want_pos = (t0 + double(first) / rate_ - e.start) * double(src_rate);
      const double drift = want_pos - v->pos;
      double step_eff = step;
      if (std::fabs(drift) > double(src_rate) * 0.25) {
        v->pos = want_pos;
        v->env = 0.0f;
      } else if (std::fabs(drift) > 0.004 * double(src_rate)) {
        const double pull = std::clamp(1.0 + drift / (0.35 * double(src_rate)), 0.985, 1.015);
        step_eff = step * pull;
      }
      const int64_t rfrom = st->from(), rend = st->end();
      const int n = last - first;
      if (!non_blocking_) {
        const int64_t want_end = int64_t(v->pos + step_eff * n) + 2;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!st->source_done() && st->end() < want_end && std::chrono::steady_clock::now() < deadline) {
          st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(v->pos)) - 4));
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      for (int i = 0; i < n; ++i, v->pos += step_eff) {
        const int64_t a = int64_t(std::floor(v->pos));
        if (a < rfrom || a >= rend) {
          if (v->env > 0.0f) {
            v->env = std::max(0.0f, v->env - env_dn);
            loc_mono_[size_t(first + i)] += 0.5f * (v->last_l + v->last_r) * v->env * base_gain;
          }
          continue;
        }
        const float frac = float(v->pos - double(a));
        float al = 0, ar = 0, bl = 0, br = 0;
        st->sample(a, al, ar);
        if (a + 1 < rend) st->sample(a + 1, bl, br);
        else {
          bl = al;
          br = ar;
        }
        const float sl = al + (bl - al) * frac;
        const float sr = ar + (br - ar) * frac;
        v->last_l = sl;
        v->last_r = sr;
        v->env = std::min(1.0f, v->env + env_up);
        loc_mono_[size_t(first + i)] += 0.5f * (sl + sr) * v->env * base_gain;
      }
      decoded_any = true;
      st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(v->pos)) - 4));
    }

    // Pan, distance-filter (PA bandpass ~250 Hz .. 2.5 kHz rolling down to ~900 Hz),
    // and mix the single decoded mono station buffer into every audible emitter referencing it.
    for (int k = 0; k < audible_count; ++k) {
      const auto& ae = audible[k];
      if (ae.station != st_idx) continue;
      float eg = emitter_gain_[ae.idx];
      if (!decoded_any) {
        for (int i = 0; i < frames; ++i) eg += (ae.target_att2 - eg) * gain_sm;
        emitter_gain_[ae.idx] = eg;
        continue;
      }
      EmitterFilter& ef = emitter_filters_[ae.idx];
      const float lp_fc = 2500.0f - 1600.0f * ae.dist_norm;
      const float lp_alpha = 1.0f - std::exp(-kTwoPi * lp_fc / float(rate_));
      for (int i = 0; i < frames; ++i) {
        eg += (ae.target_att2 - eg) * gain_sm;
        const float x = loc_mono_[size_t(i)];
        float hp = hp_alpha * (ef.hp_prev_out + x - ef.hp_prev_in);
        if (!std::isfinite(hp) || std::fabs(hp) < 1e-20f) hp = 0.0f;
        ef.hp_prev_in = x;
        ef.hp_prev_out = hp;
        ef.lp_state += (hp - ef.lp_state) * lp_alpha;
        if (!std::isfinite(ef.lp_state) || std::fabs(ef.lp_state) < 1e-20f) ef.lp_state = 0.0f;
        const float s = ef.lp_state * eg;
        out[2 * i] += s * ae.pan_l;
        out[2 * i + 1] += s * ae.pan_r;
      }
      emitter_gain_[ae.idx] = eg;
    }
  }

  for (auto& v : voices_) {
    if (v.active && v.location && !v.used) {
      release_voice(v);
    }
  }
}

void RadioPlayer::stop_homespace_voices() {
  for (auto& v : voices_) {
    if (v.active && v.homespace) release_voice(v);
  }
  homespace_gain_ = 0.0f;
}

bool RadioPlayer::render_homespace_station(int homespace_mode, float* out, int frames) {
  if (!out || frames <= 0 || homespace_mode < 1 || homespace_mode > 3) return false;
  const int st_idx = homespace_stations_[homespace_mode - 1];
  const RadioData& data = radio_.data();
  if (st_idx < 0 || size_t(st_idx) >= data.stations.size()) return false;
  StationSchedule* sched = radio_.schedule(st_idx);
  if (!sched) return false;

  for (auto& v : voices_) {
    if (v.active && v.homespace) v.used = false;
  }

  const double dt = double(frames) / double(rate_);
  const double t0 = radio_.location_clock();
  const double t1 = t0 + dt;
  const auto& st_def = data.stations[size_t(st_idx)];
  const float base_gain = float(std::clamp(st_def.volume, 0.0, 2.0) * master * std::clamp(volume, 0.0, 2.0));
  const float env_up = 1.0f / std::max(1.0f, 0.008f * float(rate_));
  const float env_dn = 1.0f / std::max(1.0f, 0.006f * float(rate_));
  const float ramp = 1.0f / (0.25f * float(rate_));
  constexpr uint64_t kHomespaceVoiceBit = 1ull << 62;

  sched->audible(t0, t1 + 1.0, scratch_);
  bool have_ready = false;
  for (const auto& e : scratch_) {
    const bool now_audible = e.start < t1 && e.end > t0;
    const int first = now_audible ? std::clamp(int(std::ceil((e.start - t0) * rate_)), 0, frames) : 0;
    const int last = now_audible ? std::clamp(int(std::ceil((e.end - t0) * rate_)), 0, frames) : 0;
    const double approx_start = now_audible ? std::max(0.0, (t0 + double(first) / rate_ - e.start) * 48000.0) : 0.0;
    Voice* v = voice_for(e.id | kHomespaceVoiceBit, e, int64_t(approx_start));
    if (!v) continue;
    v->homespace = true;
    v->used = true;
    if (!now_audible || last <= first || v->slot < 0) continue;
    PcmStream* st = banks_->pool->stream(v->slot);
    if (!st) continue;
    if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned()))
      banks_->pool->wait_ready(v->slot);
    if (st->state() != PcmStream::kReady || !st->positioned()) continue;
    have_ready = true;
    const int src_rate = st->rate();
    const double step = double(src_rate) / double(rate_);
    const double want_pos = (t0 + double(first) / rate_ - e.start) * double(src_rate);
    const double drift = want_pos - v->pos;
    double step_eff = step;
    if (std::fabs(drift) > double(src_rate) * 0.25) {
      v->pos = want_pos;
      v->env = 0.0f;
    } else if (std::fabs(drift) > 0.004 * double(src_rate)) {
      const double pull = std::clamp(1.0 + drift / (0.35 * double(src_rate)), 0.985, 1.015);
      step_eff = step * pull;
    }
    const int64_t rfrom = st->from(), rend = st->end();
    const int n = last - first;
    for (int i = 0; i < n; ++i, v->pos += step_eff) {
      homespace_gain_ = std::min(1.0f, homespace_gain_ + ramp);
      const int64_t a = int64_t(std::floor(v->pos));
      if (a < rfrom || a >= rend) {
        if (v->env > 0.0f) {
          v->env = std::max(0.0f, v->env - env_dn);
          const float g = v->env * base_gain * homespace_gain_;
          out[2 * (first + i)] += v->last_l * g;
          out[2 * (first + i) + 1] += v->last_r * g;
        }
        continue;
      }
      const float frac = float(v->pos - double(a));
      float al = 0, ar = 0, bl = 0, br = 0;
      st->sample(a, al, ar);
      if (a + 1 < rend) st->sample(a + 1, bl, br);
      else {
        bl = al;
        br = ar;
      }
      const float sl = al + (bl - al) * frac;
      const float sr = ar + (br - ar) * frac;
      v->last_l = sl;
      v->last_r = sr;
      v->env = std::min(1.0f, v->env + env_up);
      const float g = v->env * base_gain * homespace_gain_;
      out[2 * (first + i)] += sl * g;
      out[2 * (first + i) + 1] += sr * g;
    }
    st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(v->pos)) - 4));
  }

  for (auto& v : voices_) {
    if (v.active && v.homespace && !v.used) release_voice(v);
  }
  return have_ready;
}

}  // namespace audio
