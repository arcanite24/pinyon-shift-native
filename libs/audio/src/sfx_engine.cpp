#include "audio/sfx_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

#include "audio/bake.h"
#include "audio/fsb4.h"
#include "audio/stream.h"
#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

std::string lower_str(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
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

bool read_cached_sfx(const std::filesystem::path& p, PcmSample& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  int32_t rate = 0;
  uint32_t frames = 0;
  f.read(reinterpret_cast<char*>(&rate), 4);
  f.read(reinterpret_cast<char*>(&frames), 4);
  if (!f || rate <= 0 || frames == 0 || frames > 48000u * 60u) return false;
  std::vector<int16_t> s16(frames);
  f.read(reinterpret_cast<char*>(s16.data()), std::streamsize(frames * 2));
  if (!f) return false;
  out.rate = rate;
  out.data.resize(frames);
  for (size_t i = 0; i < frames; ++i) out.data[i] = float(s16[i]) / 32768.0f;
  return true;
}

void write_cached_sfx(const std::filesystem::path& p, const PcmSample& s) {
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

bool read_wav_pcm(const std::filesystem::path& p, PcmSample& out) {
  std::vector<uint8_t> raw;
  if (!read_bytes(p, raw) || raw.size() < 44) return false;
  if (std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0) return false;
  auto u16 = [&](size_t off) { return uint16_t(raw[off] | (uint16_t(raw[off + 1]) << 8)); };
  auto u32 = [&](size_t off) {
    return uint32_t(raw[off]) | (uint32_t(raw[off + 1]) << 8) | (uint32_t(raw[off + 2]) << 16) |
           (uint32_t(raw[off + 3]) << 24);
  };
  uint16_t fmt = 0, ch = 1, bits = 16;
  uint32_t rate = 48000;
  size_t pos = 12;
  while (pos + 8 <= raw.size()) {
    const uint32_t sz = u32(pos + 4);
    if (pos + 8 + sz > raw.size()) break;
    if (std::memcmp(raw.data() + pos, "fmt ", 4) == 0 && sz >= 16) {
      fmt = u16(pos + 8);
      ch = std::max<uint16_t>(1, u16(pos + 10));
      rate = u32(pos + 12);
      bits = u16(pos + 22);
    } else if (std::memcmp(raw.data() + pos, "data", 4) == 0 && fmt == 1 && bits == 16 && rate > 0) {
      const size_t frames = sz / (2 * size_t(ch));
      if (frames == 0 || frames > 48000u * 60u) return false;
      out.rate = int(rate);
      out.data.resize(frames);
      for (size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (size_t c = 0; c < ch; ++c) {
          const size_t at = pos + 8 + (i * ch + c) * 2;
          acc += double(int16_t(raw[at] | (raw[at + 1] << 8))) / 32768.0;
        }
        out.data[i] = float(acc / double(ch));
      }
      return true;
    }
    pos += 8 + sz + (sz & 1u);
  }
  return false;
}

bool read_prebaked_sfx(const std::filesystem::path& cache_dir, const std::filesystem::path& game_root,
                       const std::string& stem_low, uint32_t subsound, PcmSample& out) {
  const std::string base = stem_low + "." + std::to_string(subsound);
  std::vector<std::filesystem::path> roots;
  if (!cache_dir.empty()) {
    roots.push_back(cache_dir / "sfx");
    roots.push_back(cache_dir);
  }
  if (!game_root.empty()) {
    roots.push_back(game_root / "play" / "sound" / "sfx");
    roots.push_back(game_root.parent_path() / "play" / "sound" / "sfx");
    roots.push_back(game_root / "sound" / "sfx");
  }
  for (const auto& dir : roots) {
    if (dir.empty()) continue;
    if (read_cached_sfx(dir / (base + ".s16"), out)) return true;
    if (read_wav_pcm(dir / (base + ".wav"), out)) return true;
  }
  return false;
}

// Clean, deterministic in-process procedural SFX synthesis fallback for Android
// (where external `ffmpeg` CLI is unavailable and pre-baked PCM cache is absent).
bool synthesize_fallback_sfx(SfxSoundId id, int rate, PcmSample& out) {
  out.rate = rate;
  constexpr double kTwoPi = 6.283185307179586;
  uint32_t rng = 0x9E3779B9u ^ (uint32_t(id) * 2654435761u);
  auto next_noise = [&]() -> float {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return float(int32_t(rng)) * (1.0f / 2147483648.0f);
  };
  auto fill_tone = [&](double dur_s, double f0, double f1, float amp, double decay) {
    const size_t n = size_t(std::max(128.0, dur_s * double(rate)));
    out.data.resize(n);
    double ph = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double u = double(i) / double(n);
      const double freq = f0 + (f1 - f0) * u;
      ph += kTwoPi * freq / double(rate);
      const float env = float(std::exp(-decay * u) * std::min(1.0, double(i) / 40.0));
      out.data[i] = amp * env * float(std::sin(ph) + 0.25 * std::sin(2.0 * ph));
    }
  };
  auto fill_filtered_noise_loop = [&](double dur_s, float lp_a, float hp_a, float amp, double mod_hz = 0.0) {
    const size_t n = size_t(std::max(1024.0, dur_s * double(rate)));
    out.data.resize(n);
    float lp = 0.0f, hp = 0.0f, peak = 1e-4f;
    for (size_t i = 0; i < n; ++i) {
      const float w = next_noise();
      lp += (w - lp) * lp_a;
      hp += (lp - hp) * hp_a;
      const float mod = mod_hz > 0.0 ? (0.8f + 0.2f * std::sin(float(kTwoPi * mod_hz * double(i) / double(rate)))) : 1.0f;
      const float s = (lp - hp) * mod;
      out.data[i] = s;
      peak = std::max(peak, std::fabs(s));
    }
    const float norm = amp / peak;
    for (float& s : out.data) s *= norm;
    // Seamless loop crossfade over last 256 samples.
    const size_t xf = std::min<size_t>(256, n / 4);
    for (size_t i = 0; i < xf; ++i) {
      const float t = float(i) / float(xf);
      out.data[n - xf + i] = out.data[n - xf + i] * (1.0f - t) + out.data[i] * t;
    }
  };
  auto fill_harmonic_loop = [&](double dur_s, double base_hz, float amp, float noise_mix) {
    const size_t n = size_t(std::max(1024.0, dur_s * double(rate)));
    // Snap base_hz to exact integer cycles over n for click-free looping.
    const double cycles = std::max(1.0, std::round(base_hz * double(n) / double(rate)));
    out.data.resize(n);
    float lp = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      const double ph = kTwoPi * cycles * double(i) / double(n);
      lp += (next_noise() - lp) * 0.12f;
      const float h = 0.50f * std::sin(float(ph)) + 0.28f * std::sin(float(2.0 * ph)) +
                      0.14f * std::sin(float(3.0 * ph)) + 0.08f * std::sin(float(4.0 * ph));
      out.data[i] = amp * (h * (1.0f - noise_mix) + lp * noise_mix);
    }
  };

  switch (id) {
    case SfxSoundId::kWind:
      fill_filtered_noise_loop(1.2, 0.07f, 0.008f, 0.45f, 1.5);
      return true;
    case SfxSoundId::kTireRollOnRoad:
    case SfxSoundId::kTireRollTrackway:
      fill_filtered_noise_loop(0.8, 0.14f, 0.02f, 0.38f, 18.0);
      return true;
    case SfxSoundId::kTireRollOffRoad:
    case SfxSoundId::kTireRollGrass:
    case SfxSoundId::kTireRollBrick:
      fill_filtered_noise_loop(0.8, 0.24f, 0.04f, 0.44f, 26.0);
      return true;
    case SfxSoundId::kTireSkidOnRoad:
    case SfxSoundId::kTireSkidTrackway:
    case SfxSoundId::kTireSkidBrick:
      fill_harmonic_loop(0.6, 860.0, 0.34f, 0.45f);
      return true;
    case SfxSoundId::kTireSkidOffRoad:
    case SfxSoundId::kTireSkidGrass:
      fill_filtered_noise_loop(0.7, 0.32f, 0.06f, 0.48f, 14.0);
      return true;
    case SfxSoundId::kSurfaceHardToHard:
    case SfxSoundId::kSurfaceDirtToHard:
      fill_tone(0.11, 130.0, 62.0, 0.42f, 6.0);
      return true;
    case SfxSoundId::kTransWhine:
    case SfxSoundId::kTransWhineL1:
    case SfxSoundId::kTransWhineL2:
    case SfxSoundId::kTransWhineL3:
      fill_harmonic_loop(0.5, 420.0 + 90.0 * double(int(id) - int(SfxSoundId::kTransWhine)), 0.28f, 0.08f);
      return true;
    case SfxSoundId::kShiftStart:
    case SfxSoundId::kShiftComplete:
      fill_tone(0.08, 190.0, 95.0, 0.45f, 8.0);
      return true;
    case SfxSoundId::kTurboSpool:
    case SfxSoundId::kTurboSpoolL2:
    case SfxSoundId::kTurboSpoolL3:
      fill_harmonic_loop(0.5, 1650.0 + 220.0 * double(int(id) - int(SfxSoundId::kTurboSpool)), 0.22f, 0.28f);
      return true;
    case SfxSoundId::kTurboBovLo:
    case SfxSoundId::kTurboBovLoL2:
    case SfxSoundId::kTurboBovLoL3:
    case SfxSoundId::kTurboBovHi:
    case SfxSoundId::kTurboBovHiL2:
    case SfxSoundId::kTurboBovHiL3: {
      const size_t n = size_t(0.28 * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        lp += (next_noise() - lp) * 0.35f;
        const float flutter = 0.65f + 0.35f * std::sin(float(kTwoPi * 24.0 * u * 0.28));
        out.data[i] = lp * 0.55f * flutter * float(std::exp(-4.5 * u));
      }
      return true;
    }
    case SfxSoundId::kScWhine:
    case SfxSoundId::kScWhineL2:
    case SfxSoundId::kScWhineL3:
    case SfxSoundId::kScCscL1:
    case SfxSoundId::kScCscL2:
    case SfxSoundId::kScCscL3:
      fill_harmonic_loop(0.5, 1250.0, 0.24f, 0.10f);
      return true;
    case SfxSoundId::kEngineLfeLoop:
      fill_harmonic_loop(0.6, 58.0, 0.45f, 0.05f);
      return true;
    case SfxSoundId::kSuspensionBottom:
    case SfxSoundId::kImpactLfe:
    case SfxSoundId::kBackfireLfe:
      fill_tone(0.18, 95.0, 34.0, 0.68f, 7.0);
      return true;
    case SfxSoundId::kImpact3d:
    case SfxSoundId::kBreakProp:
    case SfxSoundId::kGlassShatter: {
      const size_t n = size_t(0.26 * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      const float alpha = (id == SfxSoundId::kGlassShatter) ? 0.65f : 0.22f;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        lp += (next_noise() - lp) * alpha;
        const float thud = (id == SfxSoundId::kGlassShatter) ? 0.0f : 0.45f * std::sin(float(kTwoPi * 75.0 * double(i) / double(rate)));
        out.data[i] = (lp * 0.65f + thud) * float(std::exp(-6.5 * u));
      }
      return true;
    }
    case SfxSoundId::kScrapeLoop:
      fill_filtered_noise_loop(0.6, 0.42f, 0.14f, 0.42f, 12.0);
      return true;
    case SfxSoundId::kWhoosh3d: {
      const size_t n = size_t(0.45 * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        const float env = float(std::sin(3.14159265 * u));
        lp += (next_noise() - lp) * (0.08f + 0.25f * env);
        out.data[i] = lp * env * 0.65f;
      }
      return true;
    }
    case SfxSoundId::kBackfirePop:
    case SfxSoundId::kBackfirePop1:
    case SfxSoundId::kBackfirePop2:
    case SfxSoundId::kBackfirePop3: {
      const size_t n = size_t(0.14 * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        lp += (next_noise() - lp) * 0.28f;
        out.data[i] = (lp * 0.8f + 0.35f * std::sin(float(kTwoPi * 110.0 * double(i) / double(rate)))) *
                      float(std::exp(-11.0 * u));
      }
      return true;
    }
    case SfxSoundId::kUiAccept:
      fill_tone(0.12, 660.0, 880.0, 0.35f, 6.0);
      return true;
    case SfxSoundId::kUiCancel:
      fill_tone(0.12, 520.0, 340.0, 0.32f, 6.5);
      return true;
    case SfxSoundId::kUiMove:
      fill_tone(0.05, 580.0, 620.0, 0.25f, 10.0);
      return true;
    case SfxSoundId::kUiError:
      fill_tone(0.18, 220.0, 175.0, 0.38f, 5.0);
      return true;
    case SfxSoundId::kUiMenuOpen:
      fill_tone(0.14, 440.0, 660.0, 0.30f, 5.5);
      return true;
    case SfxSoundId::kRewindTapeStart:
      fill_tone(0.22, 820.0, 260.0, 0.40f, 4.0);
      return true;
    case SfxSoundId::kRewindLoop:
      fill_harmonic_loop(0.5, 340.0, 0.22f, 0.35f);
      return true;
    case SfxSoundId::kCountdown321:
      fill_tone(0.24, 880.0, 880.0, 0.42f, 5.5);
      return true;
    case SfxSoundId::kSignSmash:
    case SfxSoundId::kUiSkillAward:
    case SfxSoundId::kAchievementReward:
    case SfxSoundId::kUiSpeedCamera:
      fill_tone(0.22, 740.0, 1180.0, 0.38f, 4.5);
      return true;
    case SfxSoundId::kOtherCarLoop:
    case SfxSoundId::kOtherCarLoop1:
    case SfxSoundId::kOtherCarLoop2:
    case SfxSoundId::kOtherCarLoop3:
    case SfxSoundId::kTrafficCarLoop: {
      const double hz = (id == SfxSoundId::kTrafficCarLoop)
                            ? 82.0
                            : (95.0 + 18.0 * double(int(id) - int(SfxSoundId::kOtherCarLoop)));
      fill_harmonic_loop(0.6, hz, 0.34f, 0.20f);
      return true;
    }
    default:
      return false;
  }
}

bool resolve_event_sample(const FevProject& proj, std::string_view event_path, std::filesystem::path& bank_out,
                          uint32_t& subsound_out) {
  const FevEvent* ev = proj.find_event(event_path);
  if (!ev) return false;
  auto pick_from_inst = [&](const std::vector<FevSoundInstance>& insts) -> bool {
    for (const auto& si : insts) {
      if (si.sound_def_index >= proj.sound_defs.size()) continue;
      const FevSoundDef& sd = proj.sound_defs[si.sound_def_index];
      for (const auto& en : sd.entries) {
        if (en.entry_type == 0 && !en.wavebank_name.empty()) {
          bank_out = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
          subsound_out = en.subsound_index;
          return true;
        }
      }
    }
    return false;
  };
  if (pick_from_inst(ev->simple_instances)) return true;
  for (const auto& ly : ev->layers)
    if (pick_from_inst(ly.instances)) return true;
  return false;
}

// Resolves up to `max_count` distinct (wavebank, subsound) entries across all
// sound definitions attached to `event_path` for multi-subsound variation pools.
[[maybe_unused]] std::vector<std::pair<std::filesystem::path, uint32_t>> resolve_event_samples(const FevProject& proj,
                                                                               std::string_view event_path,
                                                                               size_t max_count) {
  std::vector<std::pair<std::filesystem::path, uint32_t>> out;
  const FevEvent* ev = proj.find_event(event_path);
  if (!ev || max_count == 0) return out;
  auto collect = [&](const std::vector<FevSoundInstance>& insts) {
    for (const auto& si : insts) {
      if (out.size() >= max_count) return;
      if (si.sound_def_index >= proj.sound_defs.size()) continue;
      const FevSoundDef& sd = proj.sound_defs[si.sound_def_index];
      for (const auto& en : sd.entries) {
        if (out.size() >= max_count) return;
        if (en.entry_type != 0 || en.wavebank_name.empty()) continue;
        const auto bank = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
        bool dup = false;
        for (const auto& p : out)
          if (p.first == bank && p.second == en.subsound_index) dup = true;
        if (!dup) out.emplace_back(bank, en.subsound_index);
      }
    }
  };
  collect(ev->simple_instances);
  for (const auto& ly : ev->layers) collect(ly.instances);
  return out;
}

bool decode_fsb_subsound(const std::filesystem::path& bank_path, uint32_t subsound, const std::filesystem::path& ffmpeg,
                         const std::filesystem::path& temp_dir, PcmSample& out) {
  std::vector<uint8_t> bank_data;
  if (!read_bytes(bank_path, bank_data)) return false;
  std::vector<FsbSample> samples;
  if (!read_fsb4(bank_data, samples, nullptr) || subsound >= samples.size()) return false;
  const FsbSample& s = samples[subsound];
  if (s.offset + s.bytes > bank_data.size() || s.bytes == 0) return false;
  const int rate = s.rate > 0 ? s.rate : 48000;
  if (s.xma()) {
    if (ffmpeg.empty()) return false;
    const auto wave = xma2_wave(s, std::span<const uint8_t>(bank_data).subspan(size_t(s.offset), s.bytes));
    const auto in_p = temp_dir / ("sfx_" + std::to_string(subsound) + ".wav");
    const auto out_p = temp_dir / ("sfx_" + std::to_string(subsound) + ".s16");
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
  // MPEG fallback (e.g. streaming SFX / VO bank).
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

}  // namespace

SfxEngine::SfxEngine(int out_rate) : rate_(std::max(8000, out_rate)) {
  smooth_ = float(1.0 - std::exp(-1.0 / (0.015 * double(rate_))));
  samples_.fill(nullptr);
}

SfxEngine::~SfxEngine() {
  stop_decoder_worker();
  drain_loaded();
  for (PcmSample*& p : samples_) {
    delete p;
    p = nullptr;
  }
}

void SfxEngine::stop_decoder_worker() {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  stop_burble_worker_.store(true, std::memory_order_release);
  if (burble_worker_.joinable()) burble_worker_.join();
}

SfxSoundId SfxEngine::resolve_fallback(SfxSoundId preferred, SfxSoundId fallback) const {
  const size_t pi = size_t(preferred);
  if (pi < samples_.size() && samples_[pi] && !samples_[pi]->data.empty()) return preferred;
  return fallback;
}

void SfxEngine::drain_queue(SpscQueue<LoadedSample>& q) {
  LoadedSample ls;
  while (q.pop(ls)) {
    const size_t idx = size_t(ls.id);
    if (idx < samples_.size()) {
      // Safe: audio thread owns samples_ during render and destructor after stop.
      delete samples_[idx];
      samples_[idx] = ls.pcm;
    } else {
      delete ls.pcm;
    }
  }
}

void SfxEngine::drain_loaded() {
  drain_queue(loaded_q_);
  drain_queue(burble_loaded_q_);
  drain_queue(direct_loaded_q_);
}

void SfxEngine::set_sample(SfxSoundId id, PcmSample sample) {
  const size_t idx = size_t(id);
  if (idx >= samples_.size()) return;
  auto* p = new PcmSample(std::move(sample));
  std::lock_guard<std::mutex> lock(direct_q_mu_);
  if (!direct_loaded_q_.push({id, p})) delete p;
}

void SfxEngine::populate_procedural_fallbacks() {
  for (int i = 0; i < int(SfxSoundId::kCount); ++i) {
    const auto id = SfxSoundId(i);
    PcmSample pcm;
    if (synthesize_fallback_sfx(id, 24000, pcm) && !pcm.data.empty()) {
      set_sample(id, std::move(pcm));
    }
  }
}

int SfxEngine::loaded_sample_count() const {
  int n = 0;
  for (const PcmSample* p : samples_)
    if (p && !p->data.empty()) ++n;
  return n;
}

int SfxEngine::active_one_shots() const {
  int n = 0;
  for (const auto& o : one_shots_)
    if (o.active) ++n;
  return n;
}

bool SfxEngine::load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                     const std::string& burble_bank, std::string* note) {
  stop_decoder_worker();
  if (game_root.empty()) return false;
  const auto audio_dir = game_root / "media" / "audio";

  tires_cfg_.load(game_root, nullptr);
  coll_cfg_.load(game_root, nullptr);
  ui_map_.load(game_root, nullptr);
  backfire_cfg_.load(game_root, nullptr);
  car_rolloff_.load(game_root, false, false, nullptr);
  traffic_rolloff_.load(game_root, true, false, nullptr);

  std::vector<DecodeJob> jobs;
  auto add_from_fev = [&](const std::filesystem::path& fev_rel, SfxSoundId id, std::string_view ev_path) {
    FevProject proj;
    if (!load_fev(audio_dir / fev_rel, proj, nullptr)) return;
    std::filesystem::path bank;
    uint32_t sub = 0;
    if (resolve_event_sample(proj, ev_path, bank, sub)) jobs.push_back({id, bank, sub});
  };

  auto add_batch_fev = [&](const std::filesystem::path& fev_rel,
                           std::initializer_list<std::pair<SfxSoundId, std::string_view>> items) {
    FevProject proj;
    if (!load_fev(audio_dir / fev_rel, proj, nullptr)) return;
    for (const auto& [id, ev_path] : items) {
      std::filesystem::path bank;
      uint32_t sub = 0;
      if (resolve_event_sample(proj, ev_path, bank, sub)) jobs.push_back({id, bank, sub});
    }
  };

  add_from_fev("cars/Wind/Wind.fev", SfxSoundId::kWind, "Wind/Wind/Wind");
  add_batch_fev("cars/Tires/Tires.fev",
                {{SfxSoundId::kTireRollOnRoad, "Tires/Tires/LOD1/OnRoad/Roll_LF"},
                 {SfxSoundId::kTireSkidOnRoad, "Tires/Tires/LOD1/OnRoad/Skid_LF"},
                 {SfxSoundId::kTireRollOffRoad, "Tires/Tires/LOD1/OffRoad/Roll_LF"},
                 {SfxSoundId::kTireSkidOffRoad, "Tires/Tires/LOD1/OffRoad/Skid_LF"},
                 {SfxSoundId::kTireRollGrass, "Tires/Tires/LOD1/Grass/Roll_LF"},
                 {SfxSoundId::kTireSkidGrass, "Tires/Tires/LOD1/Grass/Skid_LF"},
                 {SfxSoundId::kTireRollBrick, "Tires/Tires/LOD1/Brick/Roll_LF"},
                 {SfxSoundId::kTireSkidBrick, "Tires/Tires/LOD1/Brick/Skid_LF"},
                 {SfxSoundId::kTireRollTrackway, "Tires/Tires/LOD1/Trackway/Roll_LF"},
                 {SfxSoundId::kTireSkidTrackway, "Tires/Tires/LOD1/Trackway/Skid_LF"},
                 {SfxSoundId::kSurfaceHardToHard, "Tires/Tires/SurfaceChanges/HardToHard"},
                 {SfxSoundId::kSurfaceDirtToHard, "Tires/Tires/SurfaceChanges/DirtToHard"}});
  add_batch_fev("cars/Transmissions/Transmissions.fev",
                {{SfxSoundId::kTransWhine, "Transmissions/Transmissions/L0"},
                 {SfxSoundId::kTransWhineL1, "Transmissions/Transmissions/L1"},
                 {SfxSoundId::kTransWhineL2, "Transmissions/Transmissions/L2"},
                 {SfxSoundId::kTransWhineL3, "Transmissions/Transmissions/L3"},
                 {SfxSoundId::kShiftStart, "Transmissions/Transmissions/ShiftStart/L0"},
                 {SfxSoundId::kShiftComplete, "Transmissions/Transmissions/ShiftComplete/L0"}});
  add_batch_fev("cars/Turbos/Turbos.fev",
                {{SfxSoundId::kTurboSpool, "Turbos/Turbos/L1 Group/L1"},
                 {SfxSoundId::kTurboSpoolL2, "Turbos/Turbos/L2 Group/L2"},
                 {SfxSoundId::kTurboSpoolL3, "Turbos/Turbos/L3 Group/L3"},
                 {SfxSoundId::kTurboBovLo, "Turbos/Turbos/L1 Group/L1_BlowOff_1_Lo"},
                 {SfxSoundId::kTurboBovHi, "Turbos/Turbos/L1 Group/L1_BlowOff_1_Hi"},
                 {SfxSoundId::kTurboBovLoL2, "Turbos/Turbos/L2 Group/L2_BlowOff_1_Lo"},
                 {SfxSoundId::kTurboBovHiL2, "Turbos/Turbos/L2 Group/L2_BlowOff_1_Hi"},
                 {SfxSoundId::kTurboBovLoL3, "Turbos/Turbos/L3 Group/L3_BlowOff_1_Lo"},
                 {SfxSoundId::kTurboBovHiL3, "Turbos/Turbos/L3 Group/L3_BlowOff_1_Hi"}});
  add_batch_fev("cars/SuperChargers/SuperChargers.fev",
                {{SfxSoundId::kScWhine, "SuperChargers/SuperChargers/Roots_TwinScrew/L1"},
                 {SfxSoundId::kScWhineL2, "SuperChargers/SuperChargers/Roots_TwinScrew/L2"},
                 {SfxSoundId::kScWhineL3, "SuperChargers/SuperChargers/Roots_TwinScrew/L3"},
                 {SfxSoundId::kScCscL1, "SuperChargers/SuperChargers/CSC/L1"},
                 {SfxSoundId::kScCscL2, "SuperChargers/SuperChargers/CSC/L2"},
                 {SfxSoundId::kScCscL3, "SuperChargers/SuperChargers/CSC/L3"}});
  add_from_fev("cars/Suspension/Suspension.fev", SfxSoundId::kSuspensionBottom, "Suspension/Suspension/BottomOut");
  add_batch_fev("cars/EngineLFE/EngineLFE.fev",
                {{SfxSoundId::kBackfireLfe, "EngineLFE/Backfire/IMPACT_LFE"},
                 {SfxSoundId::kEngineLfeLoop, "EngineLFE/EngineLFE/EngineLFE"}});
  // Fallback lookup if EngineLFE event path uses a shorter name.
  {
    FevProject lfe_fev;
    if (load_fev(audio_dir / "cars/EngineLFE/EngineLFE.fev", lfe_fev, nullptr)) {
      bool has_loop = false;
      for (const auto& j : jobs)
        if (j.id == SfxSoundId::kEngineLfeLoop) has_loop = true;
      if (!has_loop) {
        for (const auto& ev : lfe_fev.events) {
          if (lower_str(ev.path).find("backfire") == std::string::npos) {
            std::filesystem::path bank;
            uint32_t sub = 0;
            if (resolve_event_sample(lfe_fev, ev.path, bank, sub)) {
              jobs.push_back({SfxSoundId::kEngineLfeLoop, bank, sub});
              break;
            }
          }
        }
      }
    }
  }

  // Collisions via CollisionData.xml -> Collisions.fev (Impact3D, LFE, Break, Scrape3D, Glass, Whoosh3D).
  {
    FevProject coll_fev;
    if (load_fev(audio_dir / "collisions/Collisions.fev", coll_fev, nullptr)) {
      std::filesystem::path bank;
      uint32_t sub = 0;
      const auto* pair = coll_cfg_.find_pair("Asphalt", "CarAluminum");
      const std::string imp3d = pair && !pair->impact3d.empty() ? pair->impact3d
                                                                 : "Collisions/Collisions/Asphalt/CarAluminum/IMPACT_3D";
      const std::string implfe = pair && !pair->impact_lfe.empty() ? pair->impact_lfe
                                                                   : "Collisions/Collisions/LFE/IMPACT_LFE";
      if (resolve_event_sample(coll_fev, imp3d, bank, sub)) jobs.push_back({SfxSoundId::kImpact3d, bank, sub});
      if (resolve_event_sample(coll_fev, implfe, bank, sub)) jobs.push_back({SfxSoundId::kImpactLfe, bank, sub});
      if (pair && !pair->scrape3d.empty() && resolve_event_sample(coll_fev, pair->scrape3d, bank, sub)) {
        jobs.push_back({SfxSoundId::kScrapeLoop, bank, sub});
      }
      // First BreakEvent for sign/prop smashes, plus Scrape3D / Glass / Whoosh3D fallback search.
      bool has_break = false, has_scrape = false, has_glass = false, has_whoosh = false;
      for (const auto& j : jobs)
        if (j.id == SfxSoundId::kScrapeLoop) has_scrape = true;
      for (const auto& p : coll_cfg_.pairs) {
        if (!has_break && p.is_break && !p.impact3d.empty() &&
            resolve_event_sample(coll_fev, p.impact3d, bank, sub)) {
          jobs.push_back({SfxSoundId::kBreakProp, bank, sub});
          has_break = true;
        }
      }
      for (const auto& ev : coll_fev.events) {
        const std::string lp = lower_str(ev.path);
        if (!has_scrape && lp.find("scrape") != std::string::npos &&
            resolve_event_sample(coll_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kScrapeLoop, bank, sub});
          has_scrape = true;
        } else if (!has_glass && lp.find("glass") != std::string::npos &&
                   resolve_event_sample(coll_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kGlassShatter, bank, sub});
          has_glass = true;
        } else if (!has_whoosh && lp.find("whoosh") != std::string::npos &&
                   resolve_event_sample(coll_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kWhoosh3d, bank, sub});
          has_whoosh = true;
        }
      }
    }
    // Also check Wind/Cars whoosh project if Whoosh3D wasn't inside Collisions.fev.
    bool has_whoosh = false;
    for (const auto& j : jobs)
      if (j.id == SfxSoundId::kWhoosh3d) has_whoosh = true;
    if (!has_whoosh) {
      add_from_fev("cars/Whoosh/Whoosh.fev", SfxSoundId::kWhoosh3d, "Whoosh/Whoosh/Whoosh_3D");
    }
  }

  // UI, Rewind TapeStart/Loop, SkillAward, SpeedCamera, and Race Countdown via ui4audio.xml -> UIInGame.fev.
  {
    FevProject ui_fev;
    if (load_fev(audio_dir / "UI/UIInGame/UIInGame.fev", ui_fev, nullptr)) {
      auto add_ui = [&](SfxSoundId id, std::string_view play_name, std::string_view fallback_path) {
        std::string path;
        if (const auto* e = ui_map_.resolve(play_name); e && !e->is_blank()) path = e->full_path();
        if (path.empty()) path = std::string(fallback_path);
        std::filesystem::path bank;
        uint32_t sub = 0;
        if (resolve_event_sample(ui_fev, path, bank, sub)) jobs.push_back({id, bank, sub});
      };
      add_ui(SfxSoundId::kUiAccept, "Accept", "UIInGame/UI/Nav/Accept");
      add_ui(SfxSoundId::kUiCancel, "Cancel", "UIInGame/UI/Nav/Back");
      add_ui(SfxSoundId::kUiMove, "GridScroll", "UIInGame/UI/Nav/Move");
      add_ui(SfxSoundId::kUiError, "DenyAccess", "UIInGame/UI/Nav/Error");
      add_ui(SfxSoundId::kUiMenuOpen, "MenuVisible", "UIInGame/UI/Nav/SidePanel");
      add_ui(SfxSoundId::kRewindTapeStart, "Skip", "UIInGame/UI/Mulligan/TapeStart");
      add_ui(SfxSoundId::kRewindLoop, "Rewind", "UIInGame/UI/Mulligan/TapeLoop");
      add_ui(SfxSoundId::kSignSmash, "Hud_Collection", "UIInGame/UI/HorizonHud/COLLECTION");
      add_ui(SfxSoundId::kUiSkillAward, "Hud_SkillAward", "UIInGame/UI/HorizonHud/SKILL_AWARD");
      add_ui(SfxSoundId::kAchievementReward, "HUD_General_Notification_Reward",
             "UIInGame/UI/HorizonHud/HUD_General_Notification_Reward");
      add_ui(SfxSoundId::kUiSpeedCamera, "SpeedCamera", "UIInGame/UI/HorizonHud/SPEED_CAMERA");
      // Race 3-2-1 countdown & Mulligan rewind loop discovery.
      bool has_countdown = false, has_rew_loop = false;
      for (const auto& j : jobs)
        if (j.id == SfxSoundId::kRewindLoop) has_rew_loop = true;
      for (const auto& ev : ui_fev.events) {
        const std::string lp = lower_str(ev.path);
        std::filesystem::path bank;
        uint32_t sub = 0;
        if (!has_countdown && lp.find("horizoncountdowns/") != std::string::npos &&
            resolve_event_sample(ui_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kCountdown321, bank, sub});
          has_countdown = true;
        } else if (!has_rew_loop && lp.find("mulligan") != std::string::npos &&
                   lp.find("tapestart") == std::string::npos && resolve_event_sample(ui_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kRewindLoop, bank, sub});
          has_rew_loop = true;
        }
      }
    }
  }

  // Exhaust burble / backfire bank (up to 4 subsounds for randomized pop variety).
  {
    const auto burble_dir = audio_dir / "cars" / "Engines" / "Soundbanks" / "Burbles";
    std::filesystem::path bpath;
    std::error_code ec;
    if (!burble_bank.empty()) {
      const auto p = cars::mods::resolve(burble_dir / burble_bank);
      if (std::filesystem::exists(p, ec)) bpath = p;
    }
    if (bpath.empty() && std::filesystem::is_directory(burble_dir, ec)) {
      for (const auto& e : std::filesystem::directory_iterator(burble_dir, ec)) {
        if (e.is_regular_file(ec) && lower_str(e.path().extension().string()) == ".fsb") {
          bpath = e.path();
          break;
        }
      }
    }
    if (!bpath.empty()) {
      std::vector<uint8_t> raw;
      std::vector<FsbSample> fsb_s;
      const uint32_t count = (read_bytes(bpath, raw) && read_fsb4(raw, fsb_s, nullptr))
                                 ? std::min<uint32_t>(4u, uint32_t(fsb_s.size()))
                                 : 1u;
      static constexpr SfxSoundId kBurbleIds[4] = {SfxSoundId::kBackfirePop, SfxSoundId::kBackfirePop1,
                                                   SfxSoundId::kBackfirePop2, SfxSoundId::kBackfirePop3};
      for (uint32_t bi = 0; bi < count; ++bi) jobs.push_back({kBurbleIds[bi], bpath, bi});
    }
  }

  // Representative 3D opponent (up to 4 distinct banks for per-AI variety) and traffic engine loops.
  {
    const auto lod1 = audio_dir / "cars" / "Engines" / "Soundbanks" / "LOD1";
    std::error_code ec;
    std::vector<std::filesystem::path> opp_banks;
    std::filesystem::path traffic_bank;
    if (std::filesystem::is_directory(lod1, ec)) {
      for (const auto& e : std::filesystem::directory_iterator(lod1, ec)) {
        if (!e.is_regular_file(ec) || lower_str(e.path().extension().string()) != ".fsb") continue;
        const std::string fn = lower_str(e.path().filename().string());
        if (traffic_bank.empty() && fn.find("traffic") != std::string::npos) traffic_bank = e.path();
        else if (opp_banks.size() < 4 && fn.find("_exh.fsb") != std::string::npos) opp_banks.push_back(e.path());
        if (opp_banks.size() >= 4 && !traffic_bank.empty()) break;
      }
    }
    static constexpr SfxSoundId kOppIds[4] = {SfxSoundId::kOtherCarLoop, SfxSoundId::kOtherCarLoop1,
                                              SfxSoundId::kOtherCarLoop2, SfxSoundId::kOtherCarLoop3};
    for (size_t oi = 0; oi < opp_banks.size(); ++oi) jobs.push_back({kOppIds[oi], opp_banks[oi], 4});
    if (!traffic_bank.empty()) jobs.push_back({SfxSoundId::kTrafficCarLoop, traffic_bank, 2});
    else if (!opp_banks.empty()) jobs.push_back({SfxSoundId::kTrafficCarLoop, opp_banks[0], 2});
  }

  // Airborne Showcase aircraft audio from Triggerable_Events.fev / Triggerable_Events.fsb
  // (P-51 Mustang, Biplane, Helicopter, Hot-Air Balloon, Stunt Jet).
  {
    const auto trig_dir = audio_dir / "Tracks" / "Colorado";
    const auto trig_fev_path = cars::mods::resolve(trig_dir / "Triggerable_Events.fev");
    bool have_p51 = false, have_bi = false, have_heli = false, have_bal = false, have_jet = false;
    FevProject trig_fev;
    if (load_fev(trig_fev_path, trig_fev, nullptr)) {
      for (const auto& ev : trig_fev.events) {
        const std::string lp = lower_str(ev.path);
        std::filesystem::path bank;
        uint32_t sub = 0;
        if (!have_p51 && lp.find("p51") != std::string::npos && resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseP51, bank, sub});
          have_p51 = true;
          if (!have_jet) {
            jobs.push_back({SfxSoundId::kShowcaseJet, bank, sub});
            have_jet = true;
          }
        } else if (!have_bi && lp.find("biplane") != std::string::npos &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseBiplane, bank, sub});
          have_bi = true;
        } else if (!have_heli && lp.find("heli") != std::string::npos &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseHeli, bank, sub});
          have_heli = true;
        } else if (!have_bal && (lp.find("balloon") != std::string::npos || lp.find("ballon") != std::string::npos) &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseBalloon, bank, sub});
          have_bal = true;
        }
      }
    }
    const auto trig_fsb = cars::mods::resolve(trig_dir / "Triggerable_Events.fsb");
    std::error_code ec;
    if (std::filesystem::exists(trig_fsb, ec) && (!have_p51 || !have_bi || !have_heli || !have_bal)) {
      std::vector<uint8_t> raw;
      std::vector<FsbSample> fsb_s;
      if (read_bytes(trig_fsb, raw) && read_fsb4(raw, fsb_s, nullptr)) {
        for (uint32_t i = 0; i < fsb_s.size(); ++i) {
          const std::string nm = lower_str(fsb_s[i].name);
          if (!have_p51 && nm.find("p51") != std::string::npos) {
            jobs.push_back({SfxSoundId::kShowcaseP51, trig_fsb, i});
            have_p51 = true;
            if (!have_jet) {
              jobs.push_back({SfxSoundId::kShowcaseJet, trig_fsb, i});
              have_jet = true;
            }
          } else if (!have_bi && nm.find("biplane") != std::string::npos) {
            jobs.push_back({SfxSoundId::kShowcaseBiplane, trig_fsb, i});
            have_bi = true;
          } else if (!have_heli && nm.find("heli") != std::string::npos) {
            jobs.push_back({SfxSoundId::kShowcaseHeli, trig_fsb, i});
            have_heli = true;
          } else if (!have_bal && (nm.find("balloon") != std::string::npos || nm.find("baloon") != std::string::npos)) {
            jobs.push_back({SfxSoundId::kShowcaseBalloon, trig_fsb, i});
            have_bal = true;
          }
        }
      }
    }
  }

  if (note) *note = "sfx engine: queued " + std::to_string(jobs.size()) + " FEV/FSB4 sounds for async decode";
  if (!jobs.empty()) {
    start_decoder_worker(std::move(jobs), cache_dir, game_root);
  } else {
    populate_procedural_fallbacks();
  }
  return true;
}

void SfxEngine::reload_burble_bank(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                                   const std::string& burble_bank) {
  if (game_root.empty() || burble_bank.empty()) return;
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  stop_burble_worker_.store(true, std::memory_order_release);
  if (burble_worker_.joinable()) burble_worker_.join();

  const auto bpath = cars::mods::resolve(game_root / "media" / "audio" / "cars" / "Engines" / "Soundbanks" / "Burbles" /
                                         burble_bank);
  std::error_code ec;
  if (!std::filesystem::exists(bpath, ec)) return;

  std::vector<DecodeJob> jobs;
  std::vector<uint8_t> raw;
  std::vector<FsbSample> fsb_s;
  const uint32_t count = (read_bytes(bpath, raw) && read_fsb4(raw, fsb_s, nullptr))
                             ? std::min<uint32_t>(4u, uint32_t(fsb_s.size()))
                             : 1u;
  static constexpr SfxSoundId kBurbleIds[4] = {SfxSoundId::kBackfirePop, SfxSoundId::kBackfirePop1,
                                               SfxSoundId::kBackfirePop2, SfxSoundId::kBackfirePop3};
  for (uint32_t bi = 0; bi < count; ++bi) jobs.push_back({kBurbleIds[bi], bpath, bi});
  if (jobs.empty()) return;

  stop_burble_worker_.store(false, std::memory_order_release);
  burble_worker_ = std::thread([this, jobs = std::move(jobs), cache_dir, game_root]() {
    const std::filesystem::path ffmpeg = find_ffmpeg();
    std::error_code tec;
    const std::filesystem::path sfx_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "sfx");
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(tec) /
        ("shift-burble-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, tec);
    for (const auto& job : jobs) {
      if (stop_burble_worker_.load(std::memory_order_acquire)) break;
      const std::string stem_low = lower_str(job.bank_path.stem().string());
      const std::filesystem::path cache_file =
          sfx_cache.empty() ? std::filesystem::path{}
                            : (sfx_cache / (stem_low + "." + std::to_string(job.subsound) + ".s16"));
      auto pcm = std::make_unique<PcmSample>();
      bool ok = read_prebaked_sfx(cache_dir, game_root, stem_low, job.subsound, *pcm);
      if (!ok) {
        ok = decode_fsb_subsound(job.bank_path, job.subsound, ffmpeg, temp_dir, *pcm);
        if (ok && !cache_file.empty()) write_cached_sfx(cache_file, *pcm);
      }
      if (!ok) ok = synthesize_fallback_sfx(job.id, 24000, *pcm);
      if (ok && !pcm->data.empty()) {
        PcmSample* raw_pcm = pcm.release();
        if (!burble_loaded_q_.push({job.id, raw_pcm})) delete raw_pcm;
      }
    }
    std::filesystem::remove_all(temp_dir, tec);
  });
}

void SfxEngine::start_decoder_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir,
                                     std::filesystem::path game_root) {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  stop_worker_.store(false, std::memory_order_release);
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir = std::move(cache_dir),
                         game_root = std::move(game_root)]() {
    const std::filesystem::path ffmpeg = find_ffmpeg();
    std::error_code ec;
    const std::filesystem::path sfx_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "sfx");
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(ec) /
        ("shift-sfx-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, ec);
    for (const auto& job : jobs) {
      if (stop_worker_.load(std::memory_order_acquire)) break;
      const std::string stem_low = lower_str(job.bank_path.stem().string());
      const std::filesystem::path cache_file =
          sfx_cache.empty() ? std::filesystem::path{}
                            : (sfx_cache / (stem_low + "." + std::to_string(job.subsound) + ".s16"));
      auto pcm = std::make_unique<PcmSample>();
      bool ok = read_prebaked_sfx(cache_dir, game_root, stem_low, job.subsound, *pcm);
      if (!ok) {
        ok = decode_fsb_subsound(job.bank_path, job.subsound, ffmpeg, temp_dir, *pcm);
        if (ok && !cache_file.empty()) write_cached_sfx(cache_file, *pcm);
      }
      if (!ok) ok = synthesize_fallback_sfx(job.id, 24000, *pcm);
      if (ok && !pcm->data.empty()) {
        PcmSample* raw = pcm.release();
        if (!loaded_q_.push({job.id, raw})) delete raw;
      }
    }
    std::filesystem::remove_all(temp_dir, ec);
  });
}

void SfxEngine::spawn_one_shot(SfxSoundId id, Bus bus, float gain, float left, float right, float pitch) {
  if (gain <= 1e-4f) return;
  const size_t idx = size_t(id);
  if (idx >= samples_.size() || !samples_[idx] || samples_[idx]->data.empty()) return;
  OneShot* slot = nullptr;
  for (auto& o : one_shots_) {
    if (!o.active) {
      slot = &o;
      break;
    }
  }
  if (!slot) slot = &one_shots_[0];  // steal oldest slot
  slot->active = true;
  slot->id = id;
  slot->bus = bus;
  slot->phase = 0.0;
  slot->gain = gain;
  slot->left = left;
  slot->right = right;
  slot->pitch = std::clamp(pitch, 0.25f, 4.0f);
}

void SfxEngine::trigger_collision(const CollisionHit& hit, const VehicleAudioState& st) {
  drain_loaded();
  // Confirmed 75 ms rate limit (CollisionData.xml MSBetweenDiffCollisions).
  const double min_dt = coll_cfg_.ms_between_diff_collisions * 0.001;
  if (time_s_ - last_collision_s_ < min_dt) return;
  // Confirmed m/s -> mph conversion (* 2.2369, clamped to 299 mph).
  const float mph = std::clamp(hit.rel_speed_mps * 2.2369f, 0.0f, 299.0f);
  if (mph < 1.5f) return;
  last_collision_s_ = time_s_;
  const float intensity = std::clamp(mph / 45.0f, 0.15f, 1.0f);
  float pl = 0.707f, pr = 0.707f;
  compute_stereo_pan(st.listener_pos, st.listener_right, hit.pos, pl, pr);
  const float dx = hit.pos.x - st.listener_pos.x, dy = hit.pos.y - st.listener_pos.y, dz = hit.pos.z - st.listener_pos.z;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float atten = car_rolloff_.chassis_ambient.eval(dist, 1.0f);
  if (hit.is_break) {
    spawn_one_shot(SfxSoundId::kBreakProp, Bus::kAmbience, 0.75f * intensity * atten, pl, pr);
  } else {
    spawn_one_shot(SfxSoundId::kImpact3d, Bus::kAmbience, 0.85f * intensity * atten, pl, pr);
    spawn_one_shot(SfxSoundId::kImpactLfe, Bus::kAmbience, 0.65f * intensity, 0.707f, 0.707f);
    if (hit.is_car_on_car && mph > 28.0f) {
      spawn_one_shot(SfxSoundId::kGlassShatter, Bus::kAmbience, 0.50f * intensity * atten, pl, pr);
    }
  }
}

void SfxEngine::trigger_ui(UiCue cue) {
  drain_loaded();
  SfxSoundId id = SfxSoundId::kUiAccept;
  float gain = 0.7f;
  switch (cue) {
    case UiCue::kAccept: id = SfxSoundId::kUiAccept; break;
    case UiCue::kCancel: id = SfxSoundId::kUiCancel; break;
    case UiCue::kMove: id = SfxSoundId::kUiMove; gain = 0.45f; break;
    case UiCue::kError: id = SfxSoundId::kUiError; break;
    case UiCue::kMenuOpen: id = SfxSoundId::kUiMenuOpen; gain = 0.55f; break;
    case UiCue::kRewindTapeStart: id = SfxSoundId::kRewindTapeStart; gain = 0.85f; break;
    case UiCue::kCountdown321: id = SfxSoundId::kCountdown321; gain = 0.85f; break;
    case UiCue::kSignSmash: id = SfxSoundId::kSignSmash; gain = 0.80f; break;
    case UiCue::kSkillAward:
      id = resolve_fallback(SfxSoundId::kUiSkillAward, SfxSoundId::kUiAccept);
      gain = 0.70f;
      break;
    case UiCue::kAchievementReward:
      id = resolve_fallback(SfxSoundId::kAchievementReward, SfxSoundId::kSignSmash);
      gain = 0.90f;
      break;
    case UiCue::kSpeedCamera:
      id = resolve_fallback(SfxSoundId::kUiSpeedCamera,
                            resolve_fallback(SfxSoundId::kAchievementReward, SfxSoundId::kUiAccept));
      gain = 0.85f;
      break;
  }
  spawn_one_shot(id, Bus::kUi, gain, 0.707f, 0.707f);
}

void SfxEngine::mix_loop(LoopVoice& v, SfxSoundId id, float target_gain, float target_pitch, float target_l,
                         float target_r, float* bus, int frames) {
  const size_t idx = size_t(id);
  const PcmSample* pcm = idx < samples_.size() ? samples_[idx] : nullptr;
  if (!bus || !pcm || pcm->data.empty() || (target_gain <= 1e-4f && v.gain <= 1e-4f)) {
    v.gain = 0.0f;
    return;
  }
  const size_t n = pcm->data.size();
  const double rate_ratio = double(std::max(1, pcm->rate)) / double(rate_);
  const float p_clamped = std::clamp(target_pitch, 0.25f, 4.0f);
  for (int i = 0; i < frames; ++i) {
    v.gain += (target_gain - v.gain) * smooth_;
    v.pitch += (p_clamped - v.pitch) * smooth_;
    v.left += (target_l - v.left) * smooth_;
    v.right += (target_r - v.right) * smooth_;
    if (v.gain > 1e-5f) {
      const size_t k = size_t(v.phase);
      const double frac = v.phase - double(k);
      const float a = pcm->data[k % n];
      const float b = pcm->data[(k + 1) % n];
      const float s = (a + (b - a) * float(frac)) * v.gain;
      bus[2 * i] += s * v.left;
      bus[2 * i + 1] += s * v.right;
    }
    v.phase += double(v.pitch) * rate_ratio;
    if (v.phase >= double(n)) v.phase = std::fmod(v.phase, double(n));
  }
}

void SfxEngine::render(const VehicleAudioState& st, float* const* buses, int frames, EnginePlayer* focus_engine) {
  if (frames <= 0) return;
  drain_loaded();
  const double dt = double(frames) / double(rate_);
  time_s_ += dt;

  // 1. Wind (Bus::kWind)
  if (float* wind_bus = buses[int(Bus::kWind)]) {
    const float sp = std::clamp((st.speed_mph - 12.0f) / 180.0f, 0.0f, 1.0f);
    const float music_duck = st.music_active ? 0.75f : 1.0f;
    const float gain = 0.45f * std::pow(sp, 1.35f) * music_duck;
    const float pitch = 0.75f + 0.55f * std::clamp(st.speed_mph / 200.0f, 0.0f, 1.0f);
    mix_loop(wind_voice_, SfxSoundId::kWind, gain, pitch, 0.707f, 0.707f, wind_bus, frames);
  }

  // 2. Per-wheel 4-corner stereo-panned tyre roll/skid & surface changes (Bus::kTyres)
  {
    float* tyre_bus = buses[int(Bus::kTyres)];
    static constexpr float kWheelPanL[4] = {0.84f, 0.54f, 0.80f, 0.58f};
    static constexpr float kWheelPanR[4] = {0.54f, 0.84f, 0.58f, 0.80f};
    auto roll_for_surface = [&](uint8_t sgrp) -> SfxSoundId {
      switch (sgrp) {
        case 1: return resolve_fallback(SfxSoundId::kTireRollOffRoad, SfxSoundId::kTireRollOnRoad);
        case 2: return resolve_fallback(SfxSoundId::kTireRollGrass, SfxSoundId::kTireRollOnRoad);
        case 3: return resolve_fallback(SfxSoundId::kTireRollBrick, SfxSoundId::kTireRollOnRoad);
        case 4: return resolve_fallback(SfxSoundId::kTireRollTrackway, SfxSoundId::kTireRollOnRoad);
        default: return SfxSoundId::kTireRollOnRoad;
      }
    };
    auto skid_for_surface = [&](uint8_t sgrp) -> SfxSoundId {
      switch (sgrp) {
        case 1: return resolve_fallback(SfxSoundId::kTireSkidOffRoad, SfxSoundId::kTireSkidOnRoad);
        case 2: return resolve_fallback(SfxSoundId::kTireSkidGrass, SfxSoundId::kTireSkidOnRoad);
        case 3: return resolve_fallback(SfxSoundId::kTireSkidBrick, SfxSoundId::kTireSkidOnRoad);
        case 4: return resolve_fallback(SfxSoundId::kTireSkidTrackway, SfxSoundId::kTireSkidOnRoad);
        default: return SfxSoundId::kTireSkidOnRoad;
      }
    };
    for (int w = 0; w < 4; ++w) {
      const auto& wh = st.wheels[size_t(w)];
      const float pl = kWheelPanL[w];
      const float pr = kWheelPanR[w];
      if (wh.surface_group != prev_wheel_surface_[size_t(w)] && st.speed_mph > 5.0f) {
        const bool dirt_edge = (wh.surface_group == 1 || wh.surface_group == 2 ||
                                prev_wheel_surface_[size_t(w)] == 1 || prev_wheel_surface_[size_t(w)] == 2);
        const float g = 0.35f * std::clamp(st.speed_mph / 106.0f, 0.1f, 1.0f);
        spawn_one_shot(dirt_edge ? SfxSoundId::kSurfaceDirtToHard : SfxSoundId::kSurfaceHardToHard, Bus::kTyres, g, pl,
                       pr);
      }
      prev_wheel_surface_[size_t(w)] = wh.surface_group;

      // Suspension BottomOut (Bus::kEngine)
      if (wh.bottom_out_force > 0.002f && (time_s_ - susp_cooldown_s_[size_t(w)]) > 0.25) {
        susp_cooldown_s_[size_t(w)] = time_s_;
        const float sg = 0.6f * std::clamp(wh.bottom_out_force / 0.07f, 0.2f, 1.0f);
        spawn_one_shot(SfxSoundId::kSuspensionBottom, Bus::kEngine, sg, pl, pr);
      }

      if (tyre_bus) {
        const float load_f = std::clamp(wh.load, 0.0f, 1.4f);
        const float w_speed_mph = wh.wheel_rpm > 1.0f ? std::max(st.speed_mph, wh.wheel_rpm * 0.065f) : st.speed_mph;
        const float roll_g = 0.12f * std::clamp(w_speed_mph / 90.0f, 0.0f, 1.0f) * load_f;
        const float roll_p = 0.65f + 0.75f * std::clamp(w_speed_mph / 130.0f, 0.0f, 1.0f) + 0.015f * float(w);
        mix_loop(tire_roll_voices_[size_t(w)], roll_for_surface(wh.surface_group), roll_g, roll_p, pl, pr, tyre_bus,
                 frames);

        const float slip = wh.slip_combined;
        const float skid_g = (slip > 0.15f && st.speed_mph > 4.0f && load_f > 0.05f)
                                 ? 0.24f * std::clamp((slip - 0.15f) / 0.85f, 0.0f, 1.0f) * std::min(1.2f, load_f)
                                 : 0.0f;
        const float skid_p = 0.85f + 0.30f * std::clamp(slip, 0.0f, 1.0f) + 0.012f * float(w);
        mix_loop(tire_skid_voices_[size_t(w)], skid_for_surface(wh.surface_group), skid_g, skid_p, pl, pr, tyre_bus,
                 frames);
      }
    }
  }

  // 3. Transmission whine (L0-L3) + shifts, Turbo spool/BOV (L1-L3), Supercharger (Roots/CSC L1-L3),
  //    EngineLFE bass bed, Backfires (randomized 4-subsound pool) (Bus::kEngine)
  if (float* eng_bus = buses[int(Bus::kEngine)]) {
    const float rpm_span = std::max(500.0f, st.redline_rpm - st.idle_rpm);
    const float norm_rpm = std::clamp((st.rpm - st.idle_rpm) / rpm_span, 0.0f, 1.2f);

    // Transmission whine (tier-matched L0..L3) + shift start/complete.
    const int tlv = std::clamp(st.trans_level, 0, 3);
    static constexpr SfxSoundId kTransTiers[4] = {SfxSoundId::kTransWhine, SfxSoundId::kTransWhineL1,
                                                  SfxSoundId::kTransWhineL2, SfxSoundId::kTransWhineL3};
    const SfxSoundId trans_id = resolve_fallback(kTransTiers[tlv], SfxSoundId::kTransWhine);
    const float trans_g =
        (0.04f + 0.06f * float(tlv)) * std::clamp(norm_rpm, 0.0f, 1.0f) * (st.speed_mph > 2.0f ? 1.0f : 0.0f);
    const float trans_p = 0.55f + 1.15f * std::clamp(norm_rpm, 0.0f, 1.2f);
    mix_loop(trans_voice_, trans_id, trans_g, trans_p, 0.707f, 0.707f, eng_bus, frames);
    if (st.shifting && !prev_shifting_) spawn_one_shot(SfxSoundId::kShiftStart, Bus::kEngine, 0.35f, 0.707f, 0.707f);
    if (!st.shifting && prev_shifting_) spawn_one_shot(SfxSoundId::kShiftComplete, Bus::kEngine, 0.30f, 0.707f, 0.707f);
    prev_shifting_ = st.shifting;

    // Turbo spool & BOV (L1..L3, 4.5 / 13.0 PSI thresholds).
    const int tblv = std::clamp(st.turbo_level, 0, 2);
    static constexpr SfxSoundId kTurboTiers[3] = {SfxSoundId::kTurboSpool, SfxSoundId::kTurboSpoolL2,
                                                  SfxSoundId::kTurboSpoolL3};
    static constexpr SfxSoundId kBovLoTiers[3] = {SfxSoundId::kTurboBovLo, SfxSoundId::kTurboBovLoL2,
                                                  SfxSoundId::kTurboBovLoL3};
    static constexpr SfxSoundId kBovHiTiers[3] = {SfxSoundId::kTurboBovHi, SfxSoundId::kTurboBovHiL2,
                                                  SfxSoundId::kTurboBovHiL3};
    const SfxSoundId turbo_id = resolve_fallback(kTurboTiers[tblv], SfxSoundId::kTurboSpool);
    const float turbo_g = st.forced_induction == 1 ? 0.35f * std::clamp(st.turbo_norm_rpm, 0.0f, 1.0f) : 0.0f;
    const float turbo_p = 0.60f + 0.95f * std::clamp(st.turbo_norm_rpm, 0.0f, 1.2f);
    mix_loop(turbo_voice_, turbo_id, turbo_g, turbo_p, 0.707f, 0.707f, eng_bus, frames);
    if (st.forced_induction == 1 && st.bov_open && !prev_bov_open_) {
      const BovVariant v = select_bov_variant(st.turbo_psi);
      if (v == BovVariant::kHi)
        spawn_one_shot(resolve_fallback(kBovHiTiers[tblv], SfxSoundId::kTurboBovHi), Bus::kEngine, 0.55f, 0.707f,
                       0.707f);
      else if (v == BovVariant::kLo)
        spawn_one_shot(resolve_fallback(kBovLoTiers[tblv], SfxSoundId::kTurboBovLo), Bus::kEngine, 0.45f, 0.707f,
                       0.707f);
    }
    prev_bov_open_ = st.bov_open;

    // Supercharger whine (Roots/TwinScrew L1..L3 or Centrifugal CSC L1..L3).
    const int sclv = std::clamp(st.sc_level, 0, 2);
    static constexpr SfxSoundId kRootsTiers[3] = {SfxSoundId::kScWhine, SfxSoundId::kScWhineL2, SfxSoundId::kScWhineL3};
    static constexpr SfxSoundId kCscTiers[3] = {SfxSoundId::kScCscL1, SfxSoundId::kScCscL2, SfxSoundId::kScCscL3};
    const SfxSoundId sc_id = resolve_fallback(st.sc_is_centrifugal ? kCscTiers[sclv] : kRootsTiers[sclv],
                                              SfxSoundId::kScWhine);
    const float sc_g = st.forced_induction == 2
                           ? 0.40f * std::clamp(st.supercharger_norm_rpm, 0.0f, 1.0f) * (0.4f + 0.6f * st.throttle)
                           : 0.0f;
    const float sc_p = 0.55f + 1.10f * std::clamp(st.supercharger_norm_rpm, 0.0f, 1.3f);
    mix_loop(sc_voice_, sc_id, sc_g, sc_p, 0.707f, 0.707f, eng_bus, frames);

    // EngineLFE low-frequency bass bed (scales with throttle load and RPM).
    const float lfe_g =
        0.22f * std::clamp(0.25f + 0.75f * st.throttle, 0.0f, 1.0f) * std::clamp(0.35f + 0.65f * norm_rpm, 0.0f, 1.0f);
    const float lfe_p = 0.70f + 0.65f * std::clamp(norm_rpm, 0.0f, 1.1f);
    mix_loop(engine_lfe_voice_, SfxSoundId::kEngineLfeLoop, lfe_g, lfe_p, 0.707f, 0.707f, eng_bus, frames);

    // Exhaust backfire / overrun burble timer (randomized 4-subsound pool) + Expander coupling.
    const int bf_lv = std::clamp(st.backfire_level, 0, 3);
    if (bf_lv > 0 && st.throttle < 0.05f) {
      const auto& lv = backfire_cfg_.levels[size_t(bf_lv)];
      if (norm_rpm >= float(lv.min_norm_rpm) && norm_rpm <= float(lv.max_norm_rpm) && lv.max_time_ms > 0.0) {
        backfire_timer_ms_ -= dt * 1000.0;
        if (backfire_timer_ms_ <= 0.0) {
          std::uniform_real_distribution<double> tdist(std::max(50.0, lv.min_time_ms),
                                                       std::max(lv.min_time_ms + 10.0, lv.max_time_ms));
          backfire_timer_ms_ = tdist(rng_);
          std::uniform_real_distribution<double> sdist(0.0, 100.0);
          if (sdist(rng_) >= lv.silence_weighting) {
            static constexpr SfxSoundId kBurbles[4] = {SfxSoundId::kBackfirePop, SfxSoundId::kBackfirePop1,
                                                       SfxSoundId::kBackfirePop2, SfxSoundId::kBackfirePop3};
            SfxSoundId avail[4];
            int navail = 0;
            for (SfxSoundId bid : kBurbles) {
              const auto* s = samples_[size_t(bid)];
              if (s && !s->data.empty()) avail[navail++] = bid;
            }
            const SfxSoundId pop_id =
                navail > 0 ? avail[std::uniform_int_distribution<int>(0, navail - 1)(rng_)] : SfxSoundId::kBackfirePop;
            const float bg = 0.60f * float(lv.volume.present ? lv.volume.eval({norm_rpm, 0, 0, 1}) : 0.7);
            const float bpitch = 0.92f + 0.16f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
            spawn_one_shot(pop_id, Bus::kEngine, bg, 0.707f, 0.707f, bpitch);
            spawn_one_shot(SfxSoundId::kBackfireLfe, Bus::kEngine, bg * 0.65f, 0.707f, 0.707f);
            backfire_count_.fetch_add(1, std::memory_order_relaxed);
            if (focus_engine) focus_engine->trigger_expander();
          }
        }
      }
    }
  }

  // 3b. Continuous wall/car Scrape3D loop (Bus::kAmbience) & Mulligan Rewind tape loop (Bus::kUi).
  if (float* amb_bus = buses[int(Bus::kAmbience)]) {
    const float sc_g = st.scraping ? std::clamp(st.scrape_intensity, 0.15f, 1.0f) * 0.55f : 0.0f;
    const float sc_p = 0.75f + 0.45f * std::clamp(st.speed_mph / 80.0f, 0.0f, 1.0f);
    mix_loop(scrape_voice_, SfxSoundId::kScrapeLoop, sc_g, sc_p, 0.707f, 0.707f, amb_bus, frames);
  }
  if (float* ui_bus = buses[int(Bus::kUi)]) {
    const float rw_g = st.rewinding ? 0.55f : 0.0f;
    mix_loop(rewind_voice_, SfxSoundId::kRewindLoop, rw_g, 1.0f, 0.707f, 0.707f, ui_bus, frames);
  }

  // 4. 3D spatialized AI opponent (up to 4 distinct banks) & traffic car engines + near-miss Whoosh3D (Bus::kOtherCars)
  if (float* other_bus = buses[int(Bus::kOtherCars)]) {
    static constexpr SfxSoundId kOppBanks[4] = {SfxSoundId::kOtherCarLoop, SfxSoundId::kOtherCarLoop1,
                                                SfxSoundId::kOtherCarLoop2, SfxSoundId::kOtherCarLoop3};
    for (int i = 0; i < VehicleAudioState::kMaxOtherCars; ++i) {
      const auto& car = st.other_cars[size_t(i)];
      if (!car.active) {
        other_voices_[size_t(i)].gain = 0.0f;
        continue;
      }
      const Vec3f lp = clamp_audio_position(st.listener_pos);
      const Vec3f cp = clamp_audio_position(car.pos);
      const float dx = cp.x - lp.x, dy = cp.y - lp.y, dz = cp.z - lp.z;
      const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      const RolloffConfig& rc = car.is_traffic ? traffic_rolloff_ : car_rolloff_;
      const float atten = rc.chassis_exhaust_l.eval(dist, 1.0f);
      const float doppler = compute_doppler_pitch(st.listener_pos, st.listener_vel, car.pos, car.vel, 1.0f);
      float pl = 0.707f, pr = 0.707f;
      compute_stereo_pan(st.listener_pos, st.listener_right, car.pos, pl, pr);
      const float base_pitch = std::clamp(car.rpm / 3000.0f, 0.4f, 2.8f);
      const SfxSoundId sid =
          car.is_traffic ? SfxSoundId::kTrafficCarLoop
                         : resolve_fallback(kOppBanks[car.variant & 3u], SfxSoundId::kOtherCarLoop);
      mix_loop(other_voices_[size_t(i)], sid, 0.55f * atten, base_pitch * doppler, pl, pr, other_bus, frames);

      // Near-miss Whoosh3D flyby when passing another car closely at high relative speed.
      const float rvx = car.vel.x - st.listener_vel.x;
      const float rvy = car.vel.y - st.listener_vel.y;
      const float rvz = car.vel.z - st.listener_vel.z;
      const float rel_speed = std::sqrt(rvx * rvx + rvy * rvy + rvz * rvz);
      if (dist < 10.0f && rel_speed > 20.0f && (time_s_ - whoosh_cooldown_s_[size_t(i)]) > 1.5) {
        whoosh_cooldown_s_[size_t(i)] = time_s_;
        const float wg = 0.55f * std::clamp(rel_speed / 50.0f, 0.3f, 1.0f) * std::clamp(1.0f - dist / 12.0f, 0.2f, 1.0f);
        spawn_one_shot(SfxSoundId::kWhoosh3d, Bus::kOtherCars, wg, pl, pr, std::clamp(doppler, 0.75f, 1.35f));
      }
    }

    // 4b. Airborne Showcase aircraft 3D flyby audio (Triggerable_Events.fev + procedural fallback).
    if (st.showcase.active) {
      const Vec3f lp = clamp_audio_position(st.listener_pos);
      const Vec3f sp = clamp_audio_position(st.showcase.pos);
      const float dx = sp.x - lp.x, dy = sp.y - lp.y, dz = sp.z - lp.z;
      const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      // Audible up to ~280 m with crisp close-flyby presence below 90 m.
      const float atten = std::clamp(1.0f / (1.0f + (dist / 42.0f) * (dist / 42.0f)), 0.0f, 1.0f) *
                          std::clamp(1.0f - (dist - 180.0f) / 140.0f, 0.0f, 1.0f);
      const float doppler = compute_doppler_pitch(st.listener_pos, st.listener_vel, st.showcase.pos, st.showcase.vel, 1.0f);
      float pl = 0.707f, pr = 0.707f;
      compute_stereo_pan(st.listener_pos, st.listener_right, st.showcase.pos, pl, pr);
      const float maneuver_mod =
          1.0f + 0.14f * std::clamp(std::fabs(st.showcase.bank), 0.0f, 1.0f) +
          0.12f * std::clamp(st.showcase.pitch, -0.5f, 0.8f);
      const float speed_mod = 0.85f + 0.30f * std::clamp(st.showcase.speed_mps / 80.0f, 0.0f, 1.2f);
      const float target_pitch = std::clamp(speed_mod * maneuver_mod * doppler, 0.45f, 2.2f);
      const float target_gain = 0.72f * atten;

      SfxSoundId sid = SfxSoundId::kShowcaseP51;
      switch (st.showcase.kind) {
        case 1: sid = SfxSoundId::kShowcaseBalloon; break;
        case 2: sid = resolve_fallback(SfxSoundId::kShowcaseJet, SfxSoundId::kShowcaseP51); break;
        case 3: sid = resolve_fallback(SfxSoundId::kShowcaseBiplane, SfxSoundId::kShowcaseP51); break;
        case 4: sid = resolve_fallback(SfxSoundId::kShowcaseHeli, SfxSoundId::kShowcaseP51); break;
        default: sid = SfxSoundId::kShowcaseP51; break;
      }
      const PcmSample* sc_pcm = samples_[size_t(sid)];
      if (sc_pcm && !sc_pcm->data.empty()) {
        mix_loop(showcase_voice_, sid, target_gain, target_pitch, pl, pr, other_bus, frames);
      } else if (target_gain > 1e-4f || showcase_voice_.gain > 1e-4f) {
        // Clean in-process rotor / prop / turbine / burner synthesizer fallback (Android / no ffmpeg).
        constexpr double kTwoPi = 6.283185307179586;
        const double inv_rate = 1.0 / double(rate_);
        for (int f = 0; f < frames; ++f) {
          showcase_voice_.gain += (target_gain - showcase_voice_.gain) * smooth_;
          showcase_voice_.pitch += (target_pitch - showcase_voice_.pitch) * smooth_;
          showcase_voice_.left += (pl - showcase_voice_.left) * smooth_;
          showcase_voice_.right += (pr - showcase_voice_.right) * smooth_;
          showcase_synth_rng_ ^= showcase_synth_rng_ << 13;
          showcase_synth_rng_ ^= showcase_synth_rng_ >> 17;
          showcase_synth_rng_ ^= showcase_synth_rng_ << 5;
          const float noise = float(int32_t(showcase_synth_rng_)) * (1.0f / 2147483648.0f);
          float sig = 0.0f;
          if (st.showcase.kind == 4) {
            // Attack Helicopter: 14 Hz rotor blade-slap + turbine whine.
            showcase_synth_lfo_ += kTwoPi * 14.0 * double(showcase_voice_.pitch) * inv_rate;
            showcase_synth_phase_ += kTwoPi * 520.0 * double(showcase_voice_.pitch) * inv_rate;
            const float slap = 0.55f + 0.45f * std::pow(std::max(0.0f, std::sin(float(showcase_synth_lfo_))), 2.0f);
            showcase_synth_lp_ += (noise - showcase_synth_lp_) * 0.09f;
            sig = (showcase_synth_lp_ * 1.4f * slap + 0.14f * std::sin(float(showcase_synth_phase_))) * 0.55f;
          } else if (st.showcase.kind == 2) {
            // Stunt Jet: high turbine whistle + broadband jet exhaust roar.
            showcase_synth_phase_ += kTwoPi * 1180.0 * double(showcase_voice_.pitch) * inv_rate;
            showcase_synth_lp_ += (noise - showcase_synth_lp_) * 0.18f;
            sig = (showcase_synth_lp_ * 1.1f + 0.18f * std::sin(float(showcase_synth_phase_))) * 0.50f;
          } else if (st.showcase.kind == 1) {
            // Hot-Air Balloon: periodic propane burner flame roar.
            showcase_synth_lfo_ += kTwoPi * 0.35 * inv_rate;
            const float burst = std::clamp(std::sin(float(showcase_synth_lfo_)) * 2.5f - 0.8f, 0.0f, 1.0f);
            showcase_synth_lp_ += (noise - showcase_synth_lp_) * 0.12f;
            sig = showcase_synth_lp_ * (0.65f + 1.40f * burst) * 0.60f;
          } else {
            // P-51 Mustang / Biplane: harmonic piston + spinning prop blade pass.
            const double base_hz = (st.showcase.kind == 3 ? 68.0 : 92.0) * double(showcase_voice_.pitch);
            showcase_synth_phase_ += kTwoPi * base_hz * inv_rate;
            const float p1 = std::sin(float(showcase_synth_phase_));
            const float p2 = std::sin(float(showcase_synth_phase_ * 2.0));
            const float p3 = std::sin(float(showcase_synth_phase_ * 3.0));
            showcase_synth_lp_ += (noise - showcase_synth_lp_) * 0.08f;
            sig = (0.42f * p1 + 0.28f * p2 + 0.15f * p3 + 0.25f * showcase_synth_lp_) * 0.45f;
          }
          if (showcase_synth_phase_ > 1e6) showcase_synth_phase_ = std::fmod(showcase_synth_phase_, kTwoPi);
          if (showcase_synth_lfo_ > 1e6) showcase_synth_lfo_ = std::fmod(showcase_synth_lfo_, kTwoPi);
          const float out_s = sig * showcase_voice_.gain;
          other_bus[2 * f] += out_s * showcase_voice_.left;
          other_bus[2 * f + 1] += out_s * showcase_voice_.right;
        }
      }
    } else {
      showcase_voice_.gain = 0.0f;
    }
  }

  // 5. Mix active one-shot voices into their respective buses.
  for (auto& o : one_shots_) {
    if (!o.active) continue;
    const size_t idx = size_t(o.id);
    const PcmSample* pcm = idx < samples_.size() ? samples_[idx] : nullptr;
    float* bus = buses[int(o.bus)];
    if (!pcm || pcm->data.empty()) {
      o.active = false;
      continue;
    }
    const size_t n = pcm->data.size();
    const double step = double(o.pitch) * double(std::max(1, pcm->rate)) / double(rate_);
    for (int f = 0; f < frames; ++f) {
      const size_t k = size_t(o.phase);
      if (k >= n) {
        o.active = false;
        break;
      }
      if (bus) {
        const double frac = o.phase - double(k);
        const float a = pcm->data[k];
        const float b = (k + 1 < n) ? pcm->data[k + 1] : 0.0f;
        const float s = (a + (b - a) * float(frac)) * o.gain;
        bus[2 * f] += s * o.left;
        bus[2 * f + 1] += s * o.right;
      }
      o.phase += step;
    }
  }
}

}  // namespace audio
