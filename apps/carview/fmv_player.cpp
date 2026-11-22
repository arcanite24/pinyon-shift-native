#include "fmv_player.h"

#include "ui4_chrome.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "audio/bake.h"
#include "audio/xml.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace carview {
namespace {

std::string upper_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string read_text_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::filesystem::path find_ffprobe_for(const std::filesystem::path& ffmpeg) {
  if (ffmpeg.empty()) return {};
  std::error_code ec;
  auto probe = ffmpeg.parent_path() /
#ifdef _WIN32
               "ffprobe.exe";
#else
               "ffprobe";
#endif
  if (std::filesystem::exists(probe, ec)) return probe;
  return {};
}

FILE* open_pipe_read(const std::string& cmd) {
#ifdef _WIN32
  std::string wrapped = "\"" + cmd + "\"";
  return _popen(wrapped.c_str(), "rb");
#else
  return popen(cmd.c_str(), "r");
#endif
}

void close_pipe(FILE* f) {
  if (!f) return;
#ifdef _WIN32
  _pclose(f);
#else
  pclose(f);
#endif
}

std::string quote_arg(const std::string& s) { return "\"" + s + "\""; }

std::string movie_relpath(FmvMovieId id) {
  switch (id) {
    case FmvMovieId::kT10MsCombined: return "splash_intros/T10_MS_Combined.wmv";
    case FmvMovieId::kMsgsLogo: return "splash_intros/MSGS_Logo.wmv";
    case FmvMovieId::kDolbyCorona: return "splash_intros/Dolby_Corona_Intro.wmv";
    case FmvMovieId::kForzaIntro: return "splash_intros/Forza_Intro.wmv";
    case FmvMovieId::kForzaTone: return "splash_intros/Forza_Tone.wmv";
    case FmvMovieId::kPressStart: return "PressStart.wmv";
    case FmvMovieId::kFmv01Opening: return "FMV_01.wmv";
    case FmvMovieId::kFmv02Ceremony: return "FMV_02.wmv";
    case FmvMovieId::kFmv04Credits: return "FMV_04.wmv";
    case FmvMovieId::kWristbandGet: return "WristbandGet.wmv";
  }
  return "FMV_01.wmv";
}

const char* movie_label(FmvMovieId id) {
  switch (id) {
    case FmvMovieId::kT10MsCombined: return "Turn 10 / Microsoft Studios";
    case FmvMovieId::kMsgsLogo: return "Microsoft Game Studios";
    case FmvMovieId::kDolbyCorona: return "Dolby Digital";
    case FmvMovieId::kForzaIntro: return "Forza Horizon Intro";
    case FmvMovieId::kForzaTone: return "Forza Horizon Tone";
    case FmvMovieId::kPressStart: return "Press Start";
    case FmvMovieId::kFmv01Opening: return "Horizon Festival Opening";
    case FmvMovieId::kFmv02Ceremony: return "Wristband Ceremony";
    case FmvMovieId::kFmv04Credits: return "Credits";
    case FmvMovieId::kWristbandGet: return "Wristband Unlocked";
  }
  return "FMV";
}

}  // namespace

int FmvStreamDef::audio_index_for(std::string_view language) const {
  if (audio.empty()) return 0;
  const std::string want = upper_str(language.empty() ? "EN" : language);
  for (size_t i = 0; i < audio.size(); ++i) {
    if (upper_str(audio[i].lang) == want) return int(i);
  }
  for (size_t i = 0; i < audio.size(); ++i) {
    if (upper_str(audio[i].lang) == "EN") return int(i);
  }
  return 0;
}

bool parse_fmv_def(std::string_view xml, FmvStreamDef& out) {
  out = FmvStreamDef{};
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, nullptr)) return false;
  const audio::XmlNode* def = audio::iequals(root.name, "StreamDefinition") ? &root : root.child("StreamDefinition");
  if (!def) {
    for (const auto& c : root.children) {
      if (audio::iequals(c.name, "StreamDefinition")) {
        def = &c;
        break;
      }
    }
  }
  if (!def) return false;
  for (const auto& c : def->children) {
    if (audio::iequals(c.name, "Video")) {
      out.video_id = int(c.number("id", -1));
    } else if (audio::iequals(c.name, "Audio")) {
      FmvStreamDef::AudioTrack t;
      t.id = int(c.number("id", -1));
      t.lang = c.attr("lang");
      if (!t.lang.empty() || t.id >= 0) out.audio.push_back(std::move(t));
    }
  }
  return out.video_id >= 0 || !out.audio.empty();
}

bool parse_fmv_subtitles(std::string_view xml, std::vector<FmvSubtitleCue>& out) {
  out.clear();
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, nullptr)) return false;
  const audio::XmlNode* subs = audio::iequals(root.name, "Subtitles") ? &root : root.child("Subtitles");
  if (!subs) return false;
  for (const auto& c : subs->children) {
    const std::string uname = upper_str(c.name);
    if (uname.rfind("SUBTITLE", 0) != 0) continue;
    FmvSubtitleCue cue;
    cue.index = std::atoi(uname.c_str() + 8);
    cue.start_s = c.number("Start", 0.0);
    out.push_back(cue);
  }
  std::sort(out.begin(), out.end(), [](const FmvSubtitleCue& a, const FmvSubtitleCue& b) {
    return a.start_s < b.start_s;
  });
  return !out.empty();
}

FmvPlayer::~FmvPlayer() { close(); }

std::vector<FmvCatalogEntry> FmvPlayer::discover_movies(const std::filesystem::path& game_root) {
  static constexpr std::pair<FmvMovieId, bool> kCatalog[] = {
      {FmvMovieId::kT10MsCombined, true}, {FmvMovieId::kMsgsLogo, true},
      {FmvMovieId::kDolbyCorona, true},   {FmvMovieId::kForzaIntro, true},
      {FmvMovieId::kForzaTone, true},     {FmvMovieId::kPressStart, false},
      {FmvMovieId::kFmv01Opening, false}, {FmvMovieId::kFmv02Ceremony, false},
      {FmvMovieId::kFmv04Credits, false}, {FmvMovieId::kWristbandGet, false},
  };
  std::vector<FmvCatalogEntry> out;
  std::error_code ec;
  for (const auto& [id, splash] : kCatalog) {
    FmvCatalogEntry e;
    e.id = id;
    e.name = movie_label(id);
    e.is_boot_splash = splash;
    e.path = resolve_movie(game_root, id);
    e.exists = !e.path.empty() && std::filesystem::exists(e.path, ec);
    out.push_back(std::move(e));
  }
  return out;
}

std::filesystem::path FmvPlayer::resolve_movie(const std::filesystem::path& game_root, FmvMovieId id) {
  if (game_root.empty()) return {};
  const auto dir = game_root / "media" / "ui" / "videos";
  const auto p = dir / movie_relpath(id);
  std::error_code ec;
  if (std::filesystem::exists(p, ec)) return p;
  if (id == FmvMovieId::kWristbandGet) {
    const auto fb = dir / "FMV_02.wmv";
    if (std::filesystem::exists(fb, ec)) return fb;
  }
  return {};
}

bool FmvPlayer::probe(const std::filesystem::path& wmv_path, FmvProbeInfo& out) {
  out = FmvProbeInfo{};
  std::error_code ec;
  if (wmv_path.empty() || !std::filesystem::exists(wmv_path, ec)) return false;
  const auto ffmpeg = audio::find_ffmpeg();
  const auto ffprobe = find_ffprobe_for(ffmpeg);
  if (ffprobe.empty()) return false;
  const std::string cmd = quote_arg(ffprobe.string()) +
                          " -v error -show_entries stream=codec_type,width,height,r_frame_rate,duration -of default=nw=1 " +
                          quote_arg(wmv_path.string());
  FILE* pipe = open_pipe_read(cmd);
  if (!pipe) return false;
  std::string text;
  char buf[512];
  while (size_t n = std::fread(buf, 1, sizeof buf, pipe)) text.append(buf, n);
  close_pipe(pipe);

  std::istringstream ss(text);
  std::string line;
  std::string cur_type;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    if (k == "codec_type") {
      cur_type = v;
      if (cur_type == "audio") {
        out.has_audio = true;
        ++out.audio_streams;
      }
    } else if (cur_type == "video") {
      if (k == "width") out.width = std::atoi(v.c_str());
      else if (k == "height") out.height = std::atoi(v.c_str());
      else if (k == "duration") {
        const double d = std::strtod(v.c_str(), nullptr);
        if (d > out.duration_s) out.duration_s = d;
      } else if (k == "r_frame_rate") {
        const auto sl = v.find('/');
        if (sl != std::string::npos) {
          const double num = std::strtod(v.substr(0, sl).c_str(), nullptr);
          const double den = std::strtod(v.substr(sl + 1).c_str(), nullptr);
          if (num > 0.0 && den > 0.0) out.fps = num / den;
        } else {
          const double f = std::strtod(v.c_str(), nullptr);
          if (f > 0.0) out.fps = f;
        }
      }
    }
  }
  return out.width > 0 && out.height > 0;
}

bool FmvPlayer::decode_frame_at(const std::filesystem::path& wmv_path, double time_s, int target_w, int target_h,
                                std::vector<uint8_t>& out_rgba, std::string* error) {
  std::error_code ec;
  if (wmv_path.empty() || !std::filesystem::exists(wmv_path, ec)) {
    if (error) *error = "FMV file not found: " + wmv_path.string();
    return false;
  }
  const auto ffmpeg = audio::find_ffmpeg();
  if (ffmpeg.empty()) {
    if (error) *error = "ffmpeg executable not found";
    return false;
  }
  target_w = std::max(16, target_w);
  target_h = std::max(16, target_h);
  char tbuf[32];
  std::snprintf(tbuf, sizeof tbuf, "%.3f", std::max(0.0, time_s));
  const std::string cmd = quote_arg(ffmpeg.string()) + " -hide_banner -nostdin -v error -ss " + tbuf + " -i " +
                          quote_arg(wmv_path.string()) + " -map 0:v:0 -vframes 1 -vf scale=" +
                          std::to_string(target_w) + ":" + std::to_string(target_h) + " -pix_fmt rgba -f rawvideo pipe:1";
  FILE* pipe = open_pipe_read(cmd);
  if (!pipe) {
    if (error) *error = "failed to launch ffmpeg";
    return false;
  }
  const size_t need = size_t(target_w) * size_t(target_h) * 4;
  out_rgba.resize(need);
  size_t got = 0;
  while (got < need) {
    const size_t n = std::fread(out_rgba.data() + got, 1, need - got, pipe);
    if (n == 0) break;
    got += n;
  }
  close_pipe(pipe);
  if (got < need) {
    out_rgba.clear();
    if (error) *error = "ffmpeg returned truncated frame (" + std::to_string(got) + "/" + std::to_string(need) + ")";
    return false;
  }
  return true;
}

bool FmvPlayer::open(const std::filesystem::path& wmv_path, std::string_view language, int target_w, int target_h,
                     std::string* error) {
  close();
  std::error_code ec;
  if (wmv_path.empty() || !std::filesystem::exists(wmv_path, ec)) {
    if (error) *error = "FMV not found: " + wmv_path.string();
    return false;
  }
  const auto ffmpeg = audio::find_ffmpeg();
  if (ffmpeg.empty()) {
    if (error) *error = "ffmpeg not found";
    return false;
  }

  FmvProbeInfo info;
  const bool probed = probe(wmv_path, info);
  width_ = std::max(16, target_w);
  height_ = std::max(16, target_h);
  fps_ = probed && info.fps > 1.0 && info.fps <= 120.0 ? info.fps : 24.0;
  duration_s_ = probed && info.duration_s > 0.0 ? info.duration_s : 0.0;
  title_ = wmv_path.stem().string();

  // Companion .def for localized audio track selection.
  FmvStreamDef def;
  auto def_path = wmv_path;
  def_path.replace_extension(".def");
  if (std::filesystem::exists(def_path, ec)) parse_fmv_def(read_text_file(def_path), def);
  const int audio_idx = def.audio_index_for(language);

  // Companion subtitles folder: <stem_lower>_subtitles/<LANG>.xml (fallback EN.xml).
  subtitles_.clear();
  std::string stem_low = wmv_path.stem().string();
  for (char& c : stem_low) c = char(std::tolower(static_cast<unsigned char>(c)));
  const auto sub_dir = wmv_path.parent_path() / (stem_low + "_subtitles");
  if (std::filesystem::is_directory(sub_dir, ec)) {
    const std::string lang = upper_str(language.empty() ? "EN" : language);
    auto sub_file = sub_dir / (lang + ".xml");
    if (!std::filesystem::exists(sub_file, ec)) sub_file = sub_dir / "EN.xml";
    if (std::filesystem::exists(sub_file, ec)) parse_fmv_subtitles(read_text_file(sub_file), subtitles_);
  }

  current_frame_.assign(size_t(width_) * size_t(height_) * 4, 0);
  for (size_t i = 3; i < current_frame_.size(); i += 4) current_frame_[i] = 255;

  stop_.store(false, std::memory_order_release);
  video_eof_.store(false, std::memory_order_release);
  audio_eof_.store(false, std::memory_order_release);
  audio_frames_played_.store(0, std::memory_order_release);
  clock_s_ = 0.0;
  active_ = true;
  finished_ = false;
  paused_ = false;
  synthetic_ = false;
  frame_serial_ = 1;
  uploaded_serial_ = 0;

  const std::string vcmd = quote_arg(ffmpeg.string()) + " -hide_banner -nostdin -v error -i " +
                           quote_arg(wmv_path.string()) + " -map 0:v:0 -vf scale=" + std::to_string(width_) + ":" +
                           std::to_string(height_) + " -pix_fmt rgba -f rawvideo pipe:1";
  video_thread_ = std::thread(&FmvPlayer::video_worker, this, vcmd, width_, height_, fps_);

  if (!probed || info.has_audio) {
    const std::string acmd = quote_arg(ffmpeg.string()) + " -hide_banner -nostdin -v error -i " +
                             quote_arg(wmv_path.string()) + " -map 0:a:" + std::to_string(std::max(0, audio_idx)) +
                             "? -ac 2 -ar " + std::to_string(kAudioRate) + " -f f32le pipe:1";
    audio_thread_ = std::thread(&FmvPlayer::audio_worker, this, acmd);
  } else {
    audio_eof_.store(true, std::memory_order_release);
  }
  return true;
}

void FmvPlayer::open_synthetic(int w, int h, double fps, double duration_s, std::string title) {
  close();
  width_ = std::max(16, w);
  height_ = std::max(16, h);
  fps_ = std::clamp(fps, 1.0, 120.0);
  duration_s_ = std::max(0.1, duration_s);
  title_ = std::move(title);
  clock_s_ = 0.0;
  active_ = true;
  finished_ = false;
  paused_ = false;
  synthetic_ = true;
  current_frame_.resize(size_t(width_) * size_t(height_) * 4);
  render_synthetic_frame(0.0, current_frame_);
  frame_serial_ = 1;
  uploaded_serial_ = 0;
}

void FmvPlayer::close() {
  stop_.store(true, std::memory_order_release);
  if (video_thread_.joinable()) video_thread_.join();
  if (audio_thread_.joinable()) audio_thread_.join();
  {
    std::lock_guard<std::mutex> lk(video_mu_);
    video_q_.clear();
  }
  {
    std::lock_guard<std::mutex> lk(audio_mu_);
    audio_q_.clear();
  }
  active_ = false;
  finished_ = false;
  paused_ = false;
  synthetic_ = false;
  subtitles_.clear();
}

void FmvPlayer::skip() {
  if (!active_) return;
  close();
  finished_ = true;
}

int FmvPlayer::current_subtitle_index() const {
  int idx = -1;
  for (const auto& c : subtitles_) {
    if (clock_s_ >= c.start_s && clock_s_ < c.start_s + 5.0) idx = c.index;
  }
  return idx;
}

void FmvPlayer::video_worker(std::string cmd, int w, int h, double fps) {
  FILE* pipe = open_pipe_read(cmd);
  if (!pipe) {
    video_eof_.store(true, std::memory_order_release);
    return;
  }
  const size_t frame_bytes = size_t(w) * size_t(h) * 4;
  int64_t idx = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lk(video_mu_);
      if (video_q_.size() >= 6) {
        // Yield briefly while player consumes frames.
        goto wait_slot;
      }
    }
    {
      VideoFrame vf;
      vf.pts = double(idx) / fps;
      vf.rgba.resize(frame_bytes);
      size_t got = 0;
      while (got < frame_bytes && !stop_.load(std::memory_order_acquire)) {
        const size_t n = std::fread(vf.rgba.data() + got, 1, frame_bytes - got, pipe);
        if (n == 0) break;
        got += n;
      }
      if (got < frame_bytes) break;
      {
        std::lock_guard<std::mutex> lk(video_mu_);
        video_q_.push_back(std::move(vf));
      }
      ++idx;
      continue;
    }
  wait_slot:
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  close_pipe(pipe);
  video_eof_.store(true, std::memory_order_release);
}

void FmvPlayer::audio_worker(std::string cmd) {
  FILE* pipe = open_pipe_read(cmd);
  if (!pipe) {
    audio_eof_.store(true, std::memory_order_release);
    return;
  }
  constexpr size_t kChunkFloats = 2048 * 2;
  float buf[kChunkFloats];
  while (!stop_.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lk(audio_mu_);
      if (audio_q_.size() >= size_t(kAudioRate) * 2) {
        goto wait_audio;
      }
    }
    {
      const size_t n = std::fread(buf, sizeof(float), kChunkFloats, pipe);
      if (n == 0) break;
      std::lock_guard<std::mutex> lk(audio_mu_);
      audio_q_.insert(audio_q_.end(), buf, buf + n);
      continue;
    }
  wait_audio:
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  close_pipe(pipe);
  audio_eof_.store(true, std::memory_order_release);
}

void FmvPlayer::render_synthetic_frame(double t, std::vector<uint8_t>& rgba) const {
  const float phase = float(std::fmod(t * 0.5, 1.0));
  for (int y = 0; y < height_; ++y) {
    const float v = float(y) / float(std::max(1, height_ - 1));
    for (int x = 0; x < width_; ++x) {
      const float u = float(x) / float(std::max(1, width_ - 1));
      const float band = 0.5f + 0.5f * std::sin((u * 6.28318f) + phase * 6.28318f);
      uint8_t* p = &rgba[(size_t(y) * size_t(width_) + size_t(x)) * 4];
      // Horizon sunset pink/amber gradient with moving scan band.
      p[0] = uint8_t(std::clamp(int((0.18f + 0.72f * (1.0f - v) + 0.10f * band) * 255.0f), 0, 255));
      p[1] = uint8_t(std::clamp(int((0.06f + 0.25f * (1.0f - v) * band) * 255.0f), 0, 255));
      p[2] = uint8_t(std::clamp(int((0.15f + 0.45f * v + 0.15f * band) * 255.0f), 0, 255));
      p[3] = 255;
    }
  }
}

bool FmvPlayer::update(double dt, bool audio_synced) {
  if (!active_) return false;
  if (paused_) return true;

  if (synthetic_) {
    clock_s_ += std::max(0.0, dt);
    if (clock_s_ >= duration_s_) {
      close();
      finished_ = true;
      return false;
    }
    render_synthetic_frame(clock_s_, current_frame_);
    ++frame_serial_;
    return true;
  }

  const int64_t played = audio_frames_played_.load(std::memory_order_acquire);
  if (audio_synced && played > 0 && !audio_eof_.load(std::memory_order_acquire)) {
    const double a_clock = double(played) / double(kAudioRate);
    // Keep clock tightly coupled to audio clock without jumping backwards.
    clock_s_ = std::max(clock_s_, a_clock);
  } else {
    clock_s_ += std::max(0.0, dt);
  }

  bool popped = false;
  bool q_empty = false;
  {
    std::lock_guard<std::mutex> lk(video_mu_);
    while (!video_q_.empty() && video_q_.front().pts <= clock_s_ + (0.5 / fps_)) {
      current_frame_ = std::move(video_q_.front().rgba);
      video_q_.pop_front();
      popped = true;
    }
    q_empty = video_q_.empty();
  }
  if (popped) ++frame_serial_;

  if (q_empty && video_eof_.load(std::memory_order_acquire)) {
    close();
    finished_ = true;
    return false;
  }
  return true;
}

int FmvPlayer::read_audio(float* out, int frames) {
  if (!out || frames <= 0 || !active_ || paused_) return 0;
  if (synthetic_) {
    const double base = double(audio_frames_played_.fetch_add(frames, std::memory_order_relaxed));
    for (int i = 0; i < frames; ++i) {
      const float s = 0.08f * std::sin(float((base + i) * 2.0 * 3.14159265 * 440.0 / kAudioRate));
      out[2 * i] = s;
      out[2 * i + 1] = s;
    }
    return frames;
  }
  std::lock_guard<std::mutex> lk(audio_mu_);
  const size_t avail_frames = audio_q_.size() / 2;
  const int take = std::min(frames, int(avail_frames));
  for (int i = 0; i < take * 2; ++i) {
    out[i] = audio_q_.front();
    audio_q_.pop_front();
  }
  if (take > 0) audio_frames_played_.fetch_add(take, std::memory_order_relaxed);
  return take;
}

void FmvPlayer::draw(ui::List& l, float screen_w, float screen_h, std::string_view subtitle_text,
                     bool show_skip_prompt) const {
  if (!active_ || width_ <= 0 || height_ <= 0 || current_frame_.size() < size_t(width_) * size_t(height_) * 4) return;
  // Letterbox background.
  l.rect(0, 0, screen_w, screen_h, ui::rgba(0, 0, 0, 255));

  std::array<float, 4> uv{};
  bool ok = false;
  if (frame_serial_ != uploaded_serial_) {
    ok = ui::atlas_update("fmv.frame", uint32_t(width_), uint32_t(height_), current_frame_, uv);
    if (ok) uploaded_serial_ = frame_serial_;
  } else {
    ok = ui::atlas_find("fmv.frame", uv);
  }
  if (ok) {
    const float video_aspect = float(width_) / float(height_);
    const float scr_aspect = screen_w / std::max(1.0f, screen_h);
    float draw_w = screen_w, draw_h = screen_h;
    if (scr_aspect > video_aspect) {
      draw_w = screen_h * video_aspect;
    } else {
      draw_h = screen_w / video_aspect;
    }
    const float x0 = 0.5f * (screen_w - draw_w);
    const float y0 = 0.5f * (screen_h - draw_h);
    l.image(x0, y0, x0 + draw_w, y0 + draw_h, uv);
  }

  const float s = screen_h / 720.0f;
  if (!subtitle_text.empty()) {
    const float pad_y = 602.0f * s;
    ui::TextStyle ts = ui4::style(ui::Font::kA, 17.5f * s, ui4::kWhite, ui::Align::kCenter, true);
    const float sw = std::clamp(ui::List::measure(subtitle_text, ts) + 56.0f * s, 320.0f * s, screen_w * 0.78f);
    // Sharp FH1 HUDPILL01 subtitle strip (ZERO 4px rounded boxes!).
    ui4::pill(l, 0.5f * (screen_w - sw), pad_y - 6.0f * s, 0.5f * (screen_w + sw), pad_y + 32.0f * s,
              ui4::kCharcoalDark, ui4::PillKind::kHud01);
    l.text(screen_w * 0.5f, pad_y + 4.0f * s, subtitle_text, ts);
  }

  if (show_skip_prompt) {
    // Authentic BUTTON_BAR_SCENE skip prompt (ZERO 4px rounded boxes!).
    ui4::button_bar(l, {{"A", "SKIP"}}, screen_w, screen_h, 70.0f);
  }
}

}  // namespace carview
