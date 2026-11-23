#include "audio/codriver_vo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include "audio/bake.h"
#include "audio/fev.h"
#include "audio/fsb4.h"
#include "audio/stream.h"

namespace audio {
namespace {

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string upper_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool read_bytes(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

bool write_bytes(const std::filesystem::path& p, const void* data, size_t n) {
  std::ofstream f(p, std::ios::binary);
  f.write(static_cast<const char*>(data), std::streamsize(n));
  return bool(f);
}

bool read_cached_pcm(const std::filesystem::path& p, PcmSample& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  int32_t rate = 0;
  uint32_t frames = 0;
  f.read(reinterpret_cast<char*>(&rate), 4);
  f.read(reinterpret_cast<char*>(&frames), 4);
  if (!f || rate <= 0 || frames == 0 || frames > 48000u * 30u) return false;
  std::vector<int16_t> s16(frames);
  f.read(reinterpret_cast<char*>(s16.data()), std::streamsize(frames * 2));
  if (!f) return false;
  out.rate = rate;
  out.data.resize(frames);
  for (size_t i = 0; i < frames; ++i) out.data[i] = float(s16[i]) / 32768.0f;
  return true;
}

void write_cached_pcm(const std::filesystem::path& p, const PcmSample& s) {
  if (p.empty() || s.rate <= 0 || s.data.empty()) return;
  std::error_code ec;
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return;
  const int32_t rate = s.rate;
  const uint32_t frames = uint32_t(s.data.size());
  f.write(reinterpret_cast<const char*>(&rate), 4);
  f.write(reinterpret_cast<const char*>(&frames), 4);
  std::vector<int16_t> s16(frames);
  for (size_t i = 0; i < frames; ++i)
    s16[i] = int16_t(std::clamp(std::lround(double(s.data[i]) * 32767.0), -32768l, 32767l));
  f.write(reinterpret_cast<const char*>(s16.data()), std::streamsize(frames * 2));
}

bool decode_fsb_subsound(const std::vector<uint8_t>& bank_data, const std::vector<FsbSample>& samples,
                         const std::filesystem::path& bank_path, uint32_t subsound,
                         const std::filesystem::path& ffmpeg, const std::filesystem::path& temp_dir, PcmSample& out) {
  if (subsound >= samples.size()) return false;
  const FsbSample& s = samples[subsound];
  if (s.offset + s.bytes > bank_data.size() || s.bytes == 0) return false;
  const int rate = s.rate > 0 ? s.rate : 48000;
  if (s.xma()) {
    if (ffmpeg.empty()) return false;
    const auto wave = xma2_wave(s, std::span<const uint8_t>(bank_data).subspan(size_t(s.offset), s.bytes));
    const auto in_p = temp_dir / ("codriver_" + std::to_string(subsound) + ".wav");
    const auto out_p = temp_dir / ("codriver_" + std::to_string(subsound) + ".s16");
    std::error_code ec;
    std::filesystem::remove(out_p, ec);
    if (!write_bytes(in_p, wave.data(), wave.size())) return false;
    std::string cmd = "\"" + ffmpeg.string() + "\" -hide_banner -nostdin -v error -y -i \"" + in_p.string() +
                      "\" -f s16le -acodec pcm_s16le \"" + out_p.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) return false;
    std::vector<uint8_t> raw;
    if (!read_bytes(out_p, raw) || raw.size() < 2) return false;
    const size_t ch = std::max<size_t>(1, s.channels);
    const size_t frames = raw.size() / (2 * ch);
    if (frames == 0) return false;
    out.rate = rate;
    out.data.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
      double acc = 0.0;
      for (size_t c = 0; c < ch; ++c) {
        const size_t at = (i * ch + c) * 2;
        acc += double(int16_t(raw[at] | raw[at + 1] << 8)) / 32768.0;
      }
      out.data[i] = float(acc / double(ch));
    }
    std::filesystem::remove(in_p, ec);
    std::filesystem::remove(out_p, ec);
    return true;
  }
  auto src = make_fsb_mpeg_source(bank_path, s.offset, s.bytes, rate);
  if (src && src->open()) {
    out.rate = src->rate();
    out.data.clear();
    float tmp[2048 * 2];
    while (out.data.size() < size_t(out.rate) * 15) {
      const int64_t got = src->read(tmp, 2048);
      if (got <= 0) break;
      for (int64_t i = 0; i < got; ++i) out.data.push_back(0.5f * (tmp[2 * i] + tmp[2 * i + 1]));
    }
    return !out.data.empty();
  }
  return false;
}

std::string strip_project_prefix(std::string_view full_path) {
  const auto slash = full_path.find('/');
  if (slash != std::string_view::npos) {
    const auto prefix = lower_str(full_path.substr(0, slash));
    if (prefix.rfind("codriver", 0) == 0) return lower_str(full_path.substr(slash + 1));
  }
  return lower_str(full_path);
}

}  // namespace

CoDriverVoPlayer::CoDriverVoPlayer(int out_rate) : rate_(out_rate > 0 ? out_rate : 48000) {}

CoDriverVoPlayer::~CoDriverVoPlayer() {
  stop_worker();
  drain_loaded();
  for (auto* p : clips_) delete p;
  clips_.clear();
}

void CoDriverVoPlayer::stop_worker() {
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  stop_worker_.store(false, std::memory_order_release);
}

void CoDriverVoPlayer::drain_loaded() {
  LoadedClip lc{};
  while (loaded_q_.pop(lc)) {
    if (lc.clip_id >= 0 && size_t(lc.clip_id) < clips_.size()) {
      delete clips_[size_t(lc.clip_id)];
      clips_[size_t(lc.clip_id)] = lc.pcm;
    } else {
      delete lc.pcm;
    }
  }
}

void CoDriverVoPlayer::start_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir) {
  stop_worker();
  if (jobs.empty()) return;
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir = std::move(cache_dir)]() mutable {
    std::error_code ec;
    const auto temp_dir = cache_dir.empty() ? (std::filesystem::temp_directory_path(ec) / "shift_codriver")
                                            : (cache_dir / "tmp");
    std::filesystem::create_directories(temp_dir, ec);
    const auto ffmpeg = find_ffmpeg();

    std::filesystem::path cached_bank_path;
    std::vector<uint8_t> bank_data;
    std::vector<FsbSample> bank_samples;

    for (const auto& job : jobs) {
      if (stop_worker_.load(std::memory_order_acquire)) break;
      const auto cache_file = cache_dir.empty() ? std::filesystem::path{}
                                                : (cache_dir / ("clip_" + std::to_string(job.subsound) + ".pcm"));
      auto* pcm = new PcmSample();
      bool ok = !cache_file.empty() && read_cached_pcm(cache_file, *pcm);
      if (!ok) {
        if (job.bank_path != cached_bank_path) {
          cached_bank_path = job.bank_path;
          bank_data.clear();
          bank_samples.clear();
          if (read_bytes(cached_bank_path, bank_data)) read_fsb4(bank_data, bank_samples, nullptr);
        }
        ok = decode_fsb_subsound(bank_data, bank_samples, job.bank_path, job.subsound, ffmpeg, temp_dir, *pcm);
        if (ok && !cache_file.empty()) write_cached_pcm(cache_file, *pcm);
      }
      if (!ok) {
        // Android / no-ffmpeg fallback: crisp rally pace-note attention chime so stage calls are never silent.
        constexpr int kRate = 24000;
        const size_t n = size_t(0.14 * kRate);
        pcm->rate = kRate;
        pcm->data.resize(n);
        const double f = 540.0 + double((job.subsound % 5u) * 75u);
        for (size_t i = 0; i < n; ++i) {
          const double u = double(i) / double(n);
          const double env = std::sin(3.14159265 * u) * std::exp(-3.0 * u);
          pcm->data[i] = float(0.20 * env * std::sin(6.283185307179586 * f * double(i) / double(kRate)));
        }
        ok = true;
      }
      if (ok) {
        if (!loaded_q_.push({job.clip_id, pcm})) delete pcm;
      } else {
        delete pcm;
      }
    }
  });
}

bool CoDriverVoPlayer::load(const std::filesystem::path& rally_package_dir, const std::filesystem::path& cache_dir,
                            std::string_view language, std::string* note) {
  stop_worker();
  drain_loaded();
  for (auto* p : clips_) delete p;
  clips_.clear();
  cue_paths_.clear();
  path_to_id_.clear();
  clear();

  if (rally_package_dir.empty()) {
    if (note) *note = "no Rally package dir for CoDriver VO";
    return false;
  }

  const auto vo_dir = rally_package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "audio" / "VO";
  std::string lang = upper_str(language.empty() ? std::string_view("EN") : language);
  std::error_code ec;
  auto fev_path = vo_dir / ("CoDriver_" + lang + ".fev");
  auto fsb_path = vo_dir / ("CoDriver_" + lang + ".fsb");
  if (!std::filesystem::exists(fev_path, ec) || !std::filesystem::exists(fsb_path, ec)) {
    lang = "EN";
    fev_path = vo_dir / "CoDriver_EN.fev";
    fsb_path = vo_dir / "CoDriver_EN.fsb";
  }
  if (!std::filesystem::exists(fev_path, ec) || !std::filesystem::exists(fsb_path, ec)) {
    if (note) *note = "CoDriver_" + lang + ".fev/.fsb not found";
    return false;
  }

  FevProject proj;
  std::string err;
  if (!load_fev(fev_path, proj, &err)) {
    if (note) *note = "CoDriver FEV: " + err;
    return false;
  }

  language_ = lang;
  std::vector<DecodeJob> jobs;
  for (const auto& ev : proj.events) {
    uint32_t subsound = 0;
    bool found_sub = false;
    auto pick_from = [&](const std::vector<FevSoundInstance>& insts) {
      for (const auto& si : insts) {
        if (si.sound_def_index >= proj.sound_defs.size()) continue;
        for (const auto& e : proj.sound_defs[si.sound_def_index].entries) {
          if (e.entry_type == 0) {
            subsound = e.subsound_index;
            found_sub = true;
            return;
          }
        }
      }
    };
    pick_from(ev.simple_instances);
    for (const auto& layer : ev.layers)
      if (!found_sub) pick_from(layer.instances);
    if (!found_sub) continue;

    const std::string key = strip_project_prefix(ev.path);
    if (path_to_id_.count(key)) continue;
    const int id = int(cue_paths_.size());
    cue_paths_.push_back(key);
    path_to_id_[key] = id;
    clips_.push_back(nullptr);
    jobs.push_back({id, fsb_path, subsound});
  }

  const auto sub_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / ("codriver_" + lang));
  start_worker(std::move(jobs), sub_cache);
  if (note) {
    *note = "CoDriver VO (" + lang + "): " + std::to_string(cue_paths_.size()) + " cues indexed";
  }
  return !cue_paths_.empty();
}

void CoDriverVoPlayer::set_clip(std::string_view cue_path, PcmSample sample) {
  const std::string key = strip_project_prefix(cue_path);
  int id = -1;
  if (auto it = path_to_id_.find(key); it != path_to_id_.end()) {
    id = it->second;
    delete clips_[size_t(id)];
  } else {
    id = int(cue_paths_.size());
    cue_paths_.push_back(key);
    path_to_id_[key] = id;
    clips_.push_back(nullptr);
  }
  clips_[size_t(id)] = new PcmSample(std::move(sample));
}

int CoDriverVoPlayer::find_cue(std::string_view cue_path) const {
  const std::string key = strip_project_prefix(cue_path);
  auto it = path_to_id_.find(key);
  if (it != path_to_id_.end()) return it->second;
  // Suffix fallback: match trailing "/<cue>" if group differs slightly.
  const auto slash = key.rfind('/');
  const std::string tail = slash != std::string::npos ? key.substr(slash) : ("/" + key);
  for (size_t i = 0; i < cue_paths_.size(); ++i) {
    if (cue_paths_[i].size() >= tail.size() &&
        cue_paths_[i].compare(cue_paths_[i].size() - tail.size(), tail.size(), tail) == 0) {
      return int(i);
    }
  }
  return -1;
}

int CoDriverVoPlayer::loaded_clip_count() const {
  int n = 0;
  for (const auto* p : clips_)
    if (p && !p->data.empty()) ++n;
  return n;
}

void CoDriverVoPlayer::set_enabled(bool on) {
  enabled_ = on;
  if (!on) clear();
}

void CoDriverVoPlayer::enqueue_cue(std::string_view cue_path) {
  if (!enabled_) return;
  const int id = find_cue(cue_path);
  if (id >= 0) enqueue_id(id);
}

void CoDriverVoPlayer::enqueue_id(int clip_id) {
  if (!enabled_ || clip_id < 0 || size_t(clip_id) >= clips_.size()) return;
  const size_t next_tail = (pending_tail_ + 1) % kMaxPending;
  if (next_tail == pending_head_) return;  // queue full
  pending_[pending_tail_] = clip_id;
  pending_tail_ = next_tail;
}

void CoDriverVoPlayer::clear() {
  pending_head_ = pending_tail_ = 0;
  active_ = false;
  active_id_ = -1;
  phase_ = 0.0;
}

void CoDriverVoPlayer::start_next() {
  while (pending_head_ != pending_tail_) {
    const int id = pending_[pending_head_];
    pending_head_ = (pending_head_ + 1) % kMaxPending;
    if (id >= 0 && size_t(id) < clips_.size() && clips_[size_t(id)] && !clips_[size_t(id)]->data.empty()) {
      active_ = true;
      active_id_ = id;
      phase_ = 0.0;
      return;
    }
  }
  active_ = false;
  active_id_ = -1;
  phase_ = 0.0;
}

bool CoDriverVoPlayer::render(float* vo_bus, int frames) {
  drain_loaded();
  if (!enabled_ || frames <= 0) return false;
  if (!active_) start_next();
  if (!active_) return false;

  int written = 0;
  while (written < frames && active_) {
    const PcmSample* s = (active_id_ >= 0 && size_t(active_id_) < clips_.size()) ? clips_[size_t(active_id_)] : nullptr;
    if (!s || s->data.empty() || s->rate <= 0) {
      start_next();
      continue;
    }
    const double step = double(s->rate) / double(rate_);
    while (written < frames) {
      const size_t idx = size_t(phase_);
      if (idx >= s->data.size()) {
        start_next();
        break;
      }
      const float a = s->data[idx];
      const float b = (idx + 1 < s->data.size()) ? s->data[idx + 1] : a;
      const float frac = float(phase_ - double(idx));
      const float sample = (a + (b - a) * frac) * gain_;
      vo_bus[2 * written + 0] += sample;
      vo_bus[2 * written + 1] += sample;
      phase_ += step;
      ++written;
    }
  }
  return active_ || written > 0;
}

}  // namespace audio
