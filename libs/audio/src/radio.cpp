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
  // Weighted selection respecting positive likelihoods via allow predicate.
  const double target = std::uniform_real_distribution<double>(0.0, total_w)(rng_);
  double acc = 0.0;
  int chosen = 0;
  for (int i = 0; i < n; ++i) {
    acc += tod_weight(st_.tracks[size_t(i)]);
    if (acc >= target) {
      chosen = i;
      break;
    }
  }
  return tracks_.pick(rng_, [&](int k) { return k == chosen || tod_weight(st_.tracks[size_t(k)]) > 0.0; });
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
    events_.push_back({RadioKind::kIdent, clip, -1, ident_at, ident_at + ic.length, next_id_++});
    next_at = ident_at + (ic.next_track > 0 ? ic.next_track : ic.length);
  }
  const int t = pick_weighted_track();
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

void StationSchedule::audible(double t0, double t1, std::vector<RadioEvent>& out) {
  out.clear();
  for (const RadioEvent* last = last_track(); last && last->start <= t1; last = last_track()) {
    const size_t before = events_.size();
    extend();
    if (events_.size() == before) break;
  }
  const RadioEvent* keep = last_track();
  const uint64_t keep_id = keep ? keep->id : ~0ull;
  std::erase_if(events_, [&](const RadioEvent& e) { return e.end <= t0 && e.id != keep_id; });
  for (const auto& e : events_)
    if (e.start < t1 && e.end > t0) {
      out.push_back(e);
      out.back().id = e.id | station_ << 48;
    }
}

const RadioEvent* StationSchedule::track_at(double t, double* offset) {
  std::vector<RadioEvent> tmp;
  audible(t, t, tmp);
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

void Radio::select(int station) {
  for (size_t i = 0; i < data_.cycle.size(); ++i)
    if (data_.cycle[i] == station) pos_ = int(i);
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

void Radio::press(int dir) {
  const int n = int(data_.cycle.size());
  if (n == 0) return;
  select(data_.cycle[size_t(((pos_ + dir) % n + n) % n)]);
  popup_station_ = selected();
  popup_from_ = popup_clock_ + (on() ? kPopupDelay : 0.0);
}

void Radio::advance(double dt, bool run) {
  popup_clock_ += dt;
  location_clock_ += dt;
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
    if (popup_clock_ - popup_from_ > kPopupShow) {
      popup_from_ = from;
      popup_station_ = selected();
    }
  }
}

Radio::Popup Radio::popup() const {
  Popup p;
  const double age = popup_clock_ - popup_from_;
  if (age < 0 || age > kPopupShow || popup_station_ < 0) return p;
  p.alpha = std::clamp(std::min(age, kPopupShow - age) / kPopupFade, 0.0, 1.0);
  p.station = popup_station_;
  if (popup_station_ == selected() && on())
    if (auto* s = const_cast<Radio*>(this)->schedule(popup_station_))
      if (const RadioEvent* t = s->track_at(clock_)) p.track = t->track;
  return p;
}

// ------------------------------------------------------------------ player

struct RadioPlayer::Voice {
  uint64_t id = 0;
  int slot = -1;
  bool used = false;
  double pos = 0;  // source frame of the next output frame
};

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
  if (radio.on()) station_gain_[size_t(radio.selected())] = 1;
}

RadioPlayer::~RadioPlayer() {
  for (auto& v : voices_) release_voice(v);
}

int RadioPlayer::decoders() const { return int(voices_.size()); }

void RadioPlayer::release_voice(Voice& v) {
  if (v.slot >= 0 && banks_ && banks_->pool) banks_->pool->release(v.slot);
  v.slot = -1;
}

RadioPlayer::Voice* RadioPlayer::voice_for(const RadioEvent& e, int64_t initial_frame) {
  for (auto& v : voices_)
    if (v.id == e.id) return &v;
  const bool music = e.kind == RadioKind::kTrack;
  const auto& clips = music ? radio_.data().music : radio_.data().vo;
  int slot = -1;
  if (e.clip >= 0 && size_t(e.clip) < clips.size()) {
    const uint32_t sample = clips[size_t(e.clip)].sample;
    const uint64_t key = (music ? (1ull << 32) : 0ull) | uint64_t(sample);
    slot = banks_->pool->acquire(key, std::max<int64_t>(0, initial_frame));
  }
  voices_.push_back({e.id, slot, true, double(std::max<int64_t>(0, initial_frame))});
  return &voices_.back();
}

void RadioPlayer::render(float* out, int frames) {
  if (frames <= 0) return;
  const RadioData& data = radio_.data();
  const RadioLevels& lv = data.levels;
  const double dt = double(frames) / rate_;
  const int sel = radio_.on() ? radio_.selected() : -1;
  bool fading = false;
  for (size_t s = 0; s < station_gain_.size(); ++s) fading |= station_gain_[s] > 0 && int(s) != sel;
  const double t0 = radio_.clock();
  // The clock runs while on, and while the last station fades to off.
  const double t1 = t0 + (sel >= 0 || fading ? dt : 0);
  for (auto& v : voices_) v.used = false;
  mix_.resize(4 * size_t(frames));
  float* music = mix_.data();
  float* voice = mix_.data() + 2 * size_t(frames);
  const double fade_step = 1.0 / (std::max(0.01, lv.change_fade) * rate_);
  const double vol = std::clamp(volume, 0.0, 2.0);
  bool any_sel_dj = false;
  bool any_sel_festival = false;
  for (size_t s = 0; s < station_gain_.size(); ++s) {
    const double target = int(s) == sel ? 1.0 : 0.0;
    if (station_gain_[s] <= 0 && target <= 0) continue;
    StationSchedule* sched = radio_.schedule(int(s));
    if (!sched || t1 <= t0) {
      station_gain_[s] = target;
      continue;
    }
    std::fill(mix_.begin(), mix_.end(), 0.0f);
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
      if (std::fabs(expect - v->pos) > 0.05 * src_rate) {
        v->pos = expect;
        const int64_t need = int64_t(std::floor(v->pos));
        if (need < st->from() || need > st->end() + 4096) {
          st->request(need);
          if (!non_blocking_) banks_->pool->wait_ready(v->slot);
          if (!st->positioned()) continue;
        }
      }
      const int n = last - first;
      if (!non_blocking_) {
        const int64_t want_end = int64_t(v->pos + step * n) + 2;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!st->source_done() && st->end() < want_end && std::chrono::steady_clock::now() < deadline) {
          st->hint(std::max<int64_t>(st->from(), int64_t(std::floor(v->pos)) - 4));
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      const double gain = lv.master * (e.kind == RadioKind::kTrack   ? lv.music
                                       : e.kind == RadioKind::kIdent ? lv.ident
                                                                     : lv.dialogue);
      float* dst = (e.kind == RadioKind::kTrack ? music : voice) + 2 * size_t(first);
      const int64_t rfrom = st->from(), rend = st->end();
      bool missed = false;
      for (int i = 0; i < n; ++i, v->pos += step) {
        const int64_t a = int64_t(std::floor(v->pos));
        if (a < rfrom || a >= rend) {
          missed = true;
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
        dst[2 * i] += float(gain) * al;
        dst[2 * i + 1] += float(gain) * ar;
      }
      const int64_t cur = int64_t(std::floor(v->pos));
      if (missed && cur < rfrom) st->request(cur);
      else st->hint(std::max<int64_t>(rfrom, cur - 4));
    }
    const double duck_target = dj ? lv.duck : 1.0;
    if (int(s) == sel && dj) any_sel_dj = true;
    if (int(s) == sel && fest) any_sel_festival = true;
    const double duck_step = (1 - lv.duck) / (std::max(0.01, duck_target < duck_ ? lv.duck_down : lv.duck_up) * rate_);
    double g = station_gain_[s];
    for (int i = 0; i < frames; ++i) {
      g = target > g ? std::min(target, g + fade_step) : std::max(target, g - fade_step);
      if (int(s) == sel) duck_ = duck_target < duck_ ? std::max(duck_target, duck_ - duck_step)
                                                     : std::min(duck_target, duck_ + duck_step);
      const double duck = int(s) == sel ? duck_ : 1.0;
      const float k = float(g * master * vol);
      out[2 * i] += k * float(music[2 * i] * duck + voice[2 * i]);
      out[2 * i + 1] += k * float(music[2 * i + 1] * duck + voice[2 * i + 1]);
    }
    station_gain_[s] = g;
  }
  for (auto it = voices_.begin(); it != voices_.end();) {
    if (!it->used) {
      release_voice(*it);
      it = voices_.erase(it);
    } else {
      ++it;
    }
  }
  dj_speaking_ = any_sel_dj;
  festival_update_playing_ = any_sel_festival;
  // The clock runs while on, and while the last station fades to off.
  radio_.advance(dt, fading);
}

void RadioPlayer::set_location_emitters(std::vector<LocationRadioEmitter> emitters) {
  emitters_ = std::move(emitters);
}

void RadioPlayer::set_listener(std::array<float, 3> pos, float heading_rad) {
  listener_pos_ = pos;
  listener_heading_ = heading_rad;
}

void RadioPlayer::render_location_stations(float* out, int frames) {
  if (!out || frames <= 0 || emitters_.empty()) return;
  const RadioData& data = radio_.data();
  const double t0 = radio_.location_clock();
  const double dt = double(frames) / double(rate_);
  const double t1 = t0 + dt;
  const float rx = std::cos(listener_heading_);
  const float rz = -std::sin(listener_heading_);

  for (const auto& em : emitters_) {
    if (em.station < 0 || size_t(em.station) >= data.stations.size()) continue;
    const float dx = em.pos[0] - listener_pos_[0];
    const float dy = em.pos[1] - listener_pos_[1];
    const float dz = em.pos[2] - listener_pos_[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist >= em.max_distance) continue;
    const float att =
        dist <= em.min_distance ? 1.0f : std::clamp(1.0f - (dist - em.min_distance) / std::max(1.0f, em.max_distance - em.min_distance), 0.0f, 1.0f);
    const float att2 = att * att;
    const auto& st_def = data.stations[size_t(em.station)];
    const float pan_raw = dist > 1e-3f ? std::clamp((dx * rx + dz * rz) / dist, -1.0f, 1.0f) : 0.0f;
    const float pan = pan_raw * float(std::clamp(st_def.pan_level_3d, 0.0, 1.0));
    const float gl = float(st_def.volume * master * volume) * att2 * std::sqrt(0.5f * (1.0f - pan));
    const float gr = float(st_def.volume * master * volume) * att2 * std::sqrt(0.5f * (1.0f + pan));

    StationSchedule* sched = radio_.schedule(em.station);
    if (!sched) continue;
    sched->audible(t0, t1 + 1.0, scratch_);
    for (const auto& e : scratch_) {
      const bool now_audible = e.start < t1 && e.end > t0;
      const int first = now_audible ? std::clamp(int(std::ceil((e.start - t0) * rate_)), 0, frames) : 0;
      const int last = now_audible ? std::clamp(int(std::ceil((e.end - t0) * rate_)), 0, frames) : 0;
      const double approx_start = now_audible ? std::max(0.0, (t0 + double(first) / rate_ - e.start) * 48000.0) : 0.0;
      Voice* v = voice_for(e, int64_t(approx_start));
      v->used = true;
      if (!now_audible || last <= first || v->slot < 0) continue;
      PcmStream* st = banks_->pool->stream(v->slot);
      if (!st) continue;
      if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned()))
        banks_->pool->wait_ready(v->slot);
      if (st->state() != PcmStream::kReady || !st->positioned()) continue;
      const int src_rate = st->rate();
      const double step = double(src_rate) / double(rate_);
      const int64_t rfrom = st->from(), rend = st->end();
      const int n = last - first;
      for (int i = 0; i < n; ++i, v->pos += step) {
        const int64_t a = int64_t(std::floor(v->pos));
        if (a < rfrom || a >= rend) continue;
        float al = 0, ar = 0;
        st->sample(a, al, ar);
        const float mono = 0.5f * (al + ar);
        out[2 * (first + i)] += mono * gl;
        out[2 * (first + i) + 1] += mono * gr;
      }
      st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(v->pos)) - 4));
    }
  }
}

}  // namespace audio
