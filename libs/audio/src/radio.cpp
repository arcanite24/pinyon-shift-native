#include "audio/radio.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "audio/fsb4.h"
#include "audio/xml.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
    if (!iequals(node.name, "RadioStation")) continue;
    RadioStation st;
    st.name = node.attr("name");
    st.off = iequals(node.attr("isOffStation"), "true");
    if (const XmlNode* p = node.child("Playlist")) {
      st.no_repeat_tracks = int(p->number("noRepeat"));
      for (const auto& t : p->children) {
        if (!iequals(t.name, "MusicTrack")) continue;
        RadioTrack tr{t.attr("name"), t.attr("artist"), t.attr("fmodname"), -1};
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
    list("Idents", "Ident", st.idents, st.no_repeat_idents);
    list("DJRegularDialogue", "SimpleDialogue", st.dj, st.no_repeat_dj);
    out.stations.push_back(std::move(st));
  }
  int off = -1;
  for (size_t i = 0; i < out.stations.size(); ++i) {
    const auto& st = out.stations[i];
    if (st.off) off = off < 0 ? int(i) : off;
    else if (lower(st.name).find("_3d") == std::string::npos && !st.tracks.empty()) out.cycle.push_back(int(i));
  }
  if (out.cycle.empty()) return fail(error, "RadioSystem.xml has no player station");
  if (off >= 0) out.cycle.push_back(off);
  return true;
}

bool load_radio(const std::filesystem::path& game_root, RadioData& out, std::string* error,
                std::string_view language) {
  const auto dir = game_root / "media" / "audio" / "radio";
  std::string lang(language);
  if (!std::filesystem::exists(dir / ("Radio_VO_" + lang + ".fsb"))) lang = "EN";
  const auto system = dir / "RadioSystem.xml", music_info = dir / "RadioSoundbankInfo_Music.xml",
             vo_info = dir / ("RadioSoundbankInfo_VO_" + lang + ".xml");
  for (const auto& p : {system, music_info, vo_info, dir / "Radio_Music.fsb", dir / ("Radio_VO_" + lang + ".fsb")})
    if (!std::filesystem::exists(p)) return fail(error, "no " + p.string());
  if (!parse_radio(read_text(system), read_text(music_info), read_text(vo_info), out, error)) return false;
  out.music_bank = dir / "Radio_Music.fsb";
  out.vo_bank = dir / ("Radio_VO_" + lang + ".fsb");
  return true;
}

// ---------------------------------------------------------------- schedule

StationSchedule::StationSchedule(const RadioData& data, int station, uint64_t seed, double t0, bool dj)
    : data_(data), st_(data.stations[size_t(station)]), rng_(seed), use_dj_(dj), station_(uint64_t(station)) {
  tracks_.reset(int(st_.tracks.size()), st_.no_repeat_tracks);
  idents_.reset(int(st_.idents.size()), st_.no_repeat_idents);
  dj_.reset(int(st_.dj.size()), st_.no_repeat_dj);
  const int t = tracks_.pick(rng_);
  if (t < 0) return;
  const RadioClip& c = data_.music[size_t(st_.tracks[size_t(t)].clip)];
  const double until = c.ident_start > 0 ? c.ident_start : c.length;
  const double u = std::uniform_real_distribution<double>(0, std::max(0.0, until))(rng_);
  events_.push_back({RadioKind::kTrack, st_.tracks[size_t(t)].clip, t, t0 - u, t0 - u + c.length, next_id_++});
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
  const int t = tracks_.pick(rng_);
  if (t < 0) return;
  const RadioClip& tc = data_.music[size_t(st_.tracks[size_t(t)].clip)];
  events_.push_back({RadioKind::kTrack, st_.tracks[size_t(t)].clip, t, next_at, next_at + tc.length, next_id_++});
  if (use_dj_) {
    const int d = dj_.pick(rng_, [&](int k) { return data_.vo[size_t(st_.dj[size_t(k)])].length <= tc.event_start; });
    if (d >= 0) {
      const int clip = st_.dj[size_t(d)];
      events_.push_back({RadioKind::kDj, clip, -1, next_at, next_at + data_.vo[size_t(clip)].length, next_id_++});
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

Radio::Radio(const RadioData& data, uint64_t seed, bool dj) : data_(data) {
  schedules_.resize(data.stations.size());
  for (size_t i = 0; i < data.cycle.size(); ++i) {
    const int s = data.cycle[i];
    if (!data.stations[size_t(s)].off)
      schedules_[size_t(s)] = std::make_unique<StationSchedule>(data, s, seed * 0x9E3779B97F4A7C15ull + i + 1, 0.0, dj);
  }
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

void Radio::press(int dir) {
  const int n = int(data_.cycle.size());
  if (n == 0) return;
  select(data_.cycle[size_t(((pos_ + dir) % n + n) % n)]);
  popup_station_ = selected();
  popup_from_ = popup_clock_ + (on() ? kPopupDelay : 0.0);
}

void Radio::advance(double dt, bool run) {
  popup_clock_ += dt;
  if (!on()) {
    if (run) clock_ += dt;
    return;
  }
  clock_ += dt;
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

struct RadioPlayer::Decoder {
  uint64_t id = 0;
  bool used = false;
  std::vector<uint8_t> mp3;
  std::vector<uint32_t> frame_at;
  int rate = 48000, spf = 1152, channels = 1;
  drmp3 dec{};
  bool open = false;
  std::vector<float> buf;  // stereo source frames from buf_start
  int64_t buf_start = 0;
  double pos = 0;  // source frame of the next output frame
  bool done = false;

  ~Decoder() {
    if (open) drmp3_uninit(&dec);
  }
  void seek(double frame) {
    if (open) drmp3_uninit(&dec);
    open = false;
    done = frame_at.empty();
    buf.clear();
    pos = std::max(0.0, frame);
    if (done) return;
    // Two frames early, for the bit reservoir; their output is dropped.
    const int64_t k = std::clamp<int64_t>(int64_t(pos / spf) - 2, 0, int64_t(frame_at.size()) - 1);
    buf_start = k * spf;
    open = drmp3_init_memory(&dec, mp3.data() + frame_at[size_t(k)], mp3.size() - frame_at[size_t(k)], nullptr) != 0;
    done = !open;
  }
  // Decodes until source frame `last` is buffered (or the data ends).
  void fill(int64_t last) {
    float tmp[4096 * 2];
    while (!done && buf_start + int64_t(buf.size() / 2) <= last) {
      const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&dec, 4096, tmp);
      if (got == 0) {
        done = true;
        break;
      }
      const int ch = int(dec.channels);
      for (drmp3_uint64 f = 0; f < got; ++f) {
        buf.push_back(tmp[f * ch]);
        buf.push_back(tmp[f * ch + (ch > 1 ? 1 : 0)]);
      }
    }
  }
  // Adds `n` output frames at `gain` into `out`, a source step of `step`.
  void render(float* out, int n, double gain, double step) {
    const int64_t last = int64_t(pos + step * n) + 2;
    fill(last);
    for (int i = 0; i < n; ++i, pos += step) {
      const double rel = pos - double(buf_start);
      const int64_t a = int64_t(std::floor(rel));
      if (a < 0) continue;
      const double f = rel - double(a);
      const size_t ia = size_t(a) * 2;
      if (ia + 1 >= buf.size()) break;
      const bool has_b = ia + 3 < buf.size();
      const float l = float(buf[ia] + (has_b ? (buf[ia + 2] - buf[ia]) * f : 0));
      const float r = float(buf[ia + 1] + (has_b ? (buf[ia + 3] - buf[ia + 1]) * f : 0));
      out[2 * i] += float(gain) * l;
      out[2 * i + 1] += float(gain) * r;
    }
    // Keep a little behind the position.
    const int64_t drop = int64_t(pos - double(buf_start)) - 4;
    if (drop > 8192) {
      buf.erase(buf.begin(), buf.begin() + drop * 2);
      buf_start += drop;
    }
  }
};

struct RadioPlayer::Banks {
  std::vector<FsbSample> music, vo;
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

RadioPlayer::RadioPlayer(Radio& radio, int out_rate)
    : banks_(std::make_unique<Banks>()), radio_(radio), rate_(out_rate) {
  read_bank_headers(radio.data().music_bank, banks_->music);
  read_bank_headers(radio.data().vo_bank, banks_->vo);
  station_gain_.assign(radio.data().stations.size(), 0.0);
  if (radio.on()) station_gain_[size_t(radio.selected())] = 1;
}

RadioPlayer::~RadioPlayer() = default;

int RadioPlayer::decoders() const { return int(decoders_.size()); }

RadioPlayer::Decoder* RadioPlayer::decoder(const RadioEvent& e) {
  for (auto& d : decoders_)
    if (d->id == e.id) return d.get();
  const Banks& b = *banks_;
  const bool music = e.kind == RadioKind::kTrack;
  const auto& clips = music ? radio_.data().music : radio_.data().vo;
  const auto& samples = music ? b.music : b.vo;
  auto d = std::make_unique<Decoder>();
  d->id = e.id;
  const uint32_t index = clips[size_t(e.clip)].sample;
  if (index < samples.size()) {
    const FsbSample& s = samples[index];
    std::vector<uint8_t> raw(s.bytes);
    std::ifstream f(music ? radio_.data().music_bank : radio_.data().vo_bank, std::ios::binary);
    f.seekg(std::streamoff(s.offset));
    if (f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) {
      d->mp3 = mpeg_frames(raw.data(), raw.size(), &d->frame_at, &d->rate, &d->spf);
      d->channels = s.channels ? s.channels : 1;
    }
  }
  d->seek(0);
  decoders_.push_back(std::move(d));
  return decoders_.back().get();
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
  for (auto& d : decoders_) d->used = false;
  mix_.resize(4 * size_t(frames));
  float* music = mix_.data();
  float* voice = mix_.data() + 2 * size_t(frames);
  const double fade_step = 1.0 / (std::max(0.01, lv.change_fade) * rate_);
  for (size_t s = 0; s < station_gain_.size(); ++s) {
    const double target = int(s) == sel ? 1.0 : 0.0;
    if (station_gain_[s] <= 0 && target <= 0) continue;
    StationSchedule* sched = radio_.schedule(int(s));
    if (!sched || t1 <= t0) {
      station_gain_[s] = target;
      continue;
    }
    std::fill(mix_.begin(), mix_.end(), 0.0f);
    sched->audible(t0, t1, scratch_);
    bool dj = false;
    for (const auto& e : scratch_) {
      dj |= e.kind == RadioKind::kDj;
      Decoder* d = decoder(e);
      d->used = true;
      const int first = std::clamp(int(std::ceil((e.start - t0) * rate_)), 0, frames);
      const int last = std::clamp(int(std::ceil((e.end - t0) * rate_)), 0, frames);
      if (last <= first) continue;
      const double step = double(d->rate) / rate_;
      const double expect = (t0 + double(first) / rate_ - e.start) * d->rate;
      if (std::fabs(expect - d->pos) > 0.05 * d->rate) d->seek(expect);
      const double gain = lv.master * (e.kind == RadioKind::kTrack   ? lv.music
                                       : e.kind == RadioKind::kIdent ? lv.ident
                                                                     : lv.dialogue);
      d->render((e.kind == RadioKind::kTrack ? music : voice) + 2 * size_t(first), last - first, gain, step);
    }
    const double duck_target = dj ? lv.duck : 1.0;
    const double duck_step = (1 - lv.duck) / (std::max(0.01, duck_target < duck_ ? lv.duck_down : lv.duck_up) * rate_);
    double g = station_gain_[s];
    for (int i = 0; i < frames; ++i) {
      g = target > g ? std::min(target, g + fade_step) : std::max(target, g - fade_step);
      if (int(s) == sel) duck_ = duck_target < duck_ ? std::max(duck_target, duck_ - duck_step)
                                                     : std::min(duck_target, duck_ + duck_step);
      const double duck = int(s) == sel ? duck_ : 1.0;
      const float k = float(g * master);
      out[2 * i] += k * float(music[2 * i] * duck + voice[2 * i]);
      out[2 * i + 1] += k * float(music[2 * i + 1] * duck + voice[2 * i + 1]);
    }
    station_gain_[s] = g;
  }
  std::erase_if(decoders_, [](const std::unique_ptr<Decoder>& d) { return !d->used; });
  // The clock runs while on, and while the last station fades to off.
  radio_.advance(dt, fading);
}

}  // namespace audio
