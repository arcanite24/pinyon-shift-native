#include "audio/sfx_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>

#include "audio/bake.h"
#include "audio/fsb4.h"
#include "audio/stream.h"
#include "audio/xma_decode.h"
#include "audio/xml.h"
#include "cars/mods.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace audio {
namespace {

inline float safe_clamp_f(float v, float lo, float hi, float fallback = 0.0f) {
  return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

inline void lower_decoder_thread_priority() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

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
    case SfxSoundId::kRallyTireRollGravel:
    case SfxSoundId::kRallyTireRollDirt:
    case SfxSoundId::kRallyTireRollMud:
      fill_filtered_noise_loop(0.8, 0.24f, 0.04f, 0.44f, 26.0);
      return true;
    case SfxSoundId::kRallyTireRollTarmac:
      fill_filtered_noise_loop(0.8, 0.14f, 0.02f, 0.38f, 18.0);
      return true;
    case SfxSoundId::kTireSkidOnRoad:
    case SfxSoundId::kTireSkidTrackway:
    case SfxSoundId::kTireSkidBrick:
    case SfxSoundId::kRallyTireSkidTarmac:
      fill_harmonic_loop(0.6, 860.0, 0.34f, 0.45f);
      return true;
    case SfxSoundId::kTireSkidOffRoad:
    case SfxSoundId::kTireSkidGrass:
    case SfxSoundId::kRallyTireSkidGravel:
    case SfxSoundId::kRallyTireSkidDirt:
    case SfxSoundId::kRallyTireSkidMud:
      fill_filtered_noise_loop(0.7, 0.32f, 0.06f, 0.48f, 14.0);
      return true;
    case SfxSoundId::kSurfaceHardToHard:
    case SfxSoundId::kSurfaceDirtToHard:
      fill_tone(0.11, 130.0, 62.0, 0.42f, 6.0);
      return true;
    case SfxSoundId::kSurfaceLeavesOn:
    case SfxSoundId::kSurfaceLeavesOff:
      fill_filtered_noise_loop(0.16, 0.46f, 0.22f, 0.36f, 34.0);
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
    case SfxSoundId::kElectricMotor:
      fill_harmonic_loop(0.5, 280.0, 0.26f, 0.04f);
      return true;
    case SfxSoundId::kElectricMotorMid:
      fill_harmonic_loop(0.5, 640.0, 0.22f, 0.03f);
      return true;
    case SfxSoundId::kElectricMotorHigh:
      fill_harmonic_loop(0.5, 1380.0, 0.16f, 0.02f);
      return true;
    case SfxSoundId::kExhNoise:
      fill_filtered_noise_loop(0.8, 0.28f, 0.10f, 0.34f, 11.0);
      return true;
    case SfxSoundId::kEngineDamageLoop: {
      const size_t n = size_t(0.6 * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / double(rate);
        const double cyc = std::fmod(t * 22.0, 1.0);
        const float knock = float(std::exp(-28.0 * cyc) * std::sin(kTwoPi * 760.0 * t));
        lp += (next_noise() - lp) * 0.32f;
        out.data[i] = 0.38f * knock + 0.18f * lp;
      }
      return true;
    }
    case SfxSoundId::kEngineMisfire:
      fill_tone(0.14, 155.0, 65.0, 0.48f, 9.0);
      return true;
    case SfxSoundId::kRoadsideReflection:
      fill_filtered_noise_loop(0.6, 0.24f, 0.16f, 0.30f, 5.0);
      return true;
    case SfxSoundId::kActiveAero:
      fill_tone(0.32, 320.0, 640.0, 0.32f, 3.8);
      return true;
    case SfxSoundId::kSuspensionBottom:
    case SfxSoundId::kImpactLfe:
    case SfxSoundId::kBackfireLfe:
    case SfxSoundId::kCarLandingLight:
    case SfxSoundId::kCarLandingHeavy:
    case SfxSoundId::kCarLandingDirt:
      fill_tone(id == SfxSoundId::kCarLandingHeavy ? 0.24 : 0.18,
                id == SfxSoundId::kCarLandingHeavy ? 110.0 : 95.0, 32.0,
                id == SfxSoundId::kCarLandingHeavy ? 0.78f : 0.65f, 6.5);
      return true;
    case SfxSoundId::kImpact3d:
    case SfxSoundId::kBreakProp:
    case SfxSoundId::kBreakWood:
    case SfxSoundId::kBreakMetal:
    case SfxSoundId::kBreakGlass:
    case SfxSoundId::kBreakPlastic:
    case SfxSoundId::kBreakFoliage:
    case SfxSoundId::kBreakStone:
    case SfxSoundId::kBreakFence:
    case SfxSoundId::kGlassShatter:
    case SfxSoundId::kGlassSideWindow:
    case SfxSoundId::kGlassRearWindow:
    case SfxSoundId::kGlassTaillight:
    case SfxSoundId::kGlassLexan: {
      const bool glass = (id == SfxSoundId::kGlassShatter || id == SfxSoundId::kBreakGlass ||
                          id == SfxSoundId::kGlassSideWindow || id == SfxSoundId::kGlassRearWindow ||
                          id == SfxSoundId::kGlassTaillight || id == SfxSoundId::kGlassLexan);
      const bool metal = (id == SfxSoundId::kBreakMetal || id == SfxSoundId::kBreakFence);
      const bool foliage = (id == SfxSoundId::kBreakFoliage);
      const size_t n = size_t((foliage ? 0.20 : 0.26) * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      const float alpha = (id == SfxSoundId::kGlassTaillight) ? 0.78f
                          : (id == SfxSoundId::kGlassLexan)   ? 0.50f
                          : glass                             ? 0.65f
                          : metal                             ? 0.48f
                          : foliage                           ? 0.36f
                                                              : 0.22f;
      const double tone_hz = metal ? 420.0 : (id == SfxSoundId::kBreakPlastic ? 210.0 : 75.0);
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        lp += (next_noise() - lp) * alpha;
        const float thud = (glass || foliage) ? 0.0f : 0.45f * std::sin(float(kTwoPi * tone_hz * double(i) / double(rate)));
        out.data[i] = (lp * 0.65f + thud) * float(std::exp(-6.5 * u));
      }
      return true;
    }
    case SfxSoundId::kScrapeLoop:
    case SfxSoundId::kScrapeMetal:
    case SfxSoundId::kScrapeDirt:
    case SfxSoundId::kScrapeWood: {
      const float lp_a = (id == SfxSoundId::kScrapeMetal) ? 0.54f : (id == SfxSoundId::kScrapeDirt ? 0.28f : 0.42f);
      fill_filtered_noise_loop(0.6, lp_a, 0.14f, 0.42f, 12.0);
      return true;
    }
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
    case SfxSoundId::kBurblePop0:
    case SfxSoundId::kBurblePop1:
    case SfxSoundId::kBurblePop2:
    case SfxSoundId::kBurblePop3: {
      const size_t n = size_t(0.075 * double(rate));
      out.data.resize(n);
      float lp = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        lp += (next_noise() - lp) * 0.36f;
        out.data[i] = (lp * 0.65f + 0.22f * std::sin(float(kTwoPi * 165.0 * double(i) / double(rate)))) *
                      float(std::exp(-14.0 * u));
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
    case SfxSoundId::kRewindTapeStop:
      fill_tone(0.16, 260.0, 780.0, 0.40f, 5.0);
      return true;
    case SfxSoundId::kRewindLoop:
      fill_harmonic_loop(0.5, 340.0, 0.22f, 0.35f);
      return true;
    case SfxSoundId::kCountdown321:
      fill_tone(0.24, 880.0, 880.0, 0.42f, 5.5);
      return true;
    case SfxSoundId::kUiCountdownTick:
      fill_tone(0.12, 880.0, 880.0, 0.38f, 8.5);
      return true;
    case SfxSoundId::kUiLap: {
      const size_t n = size_t(0.26 * double(rate));
      out.data.resize(n);
      double ph = 0.0;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        const double f = u < 0.45 ? 784.0 : 1046.5;
        ph += kTwoPi * f / double(rate);
        const float env = float(std::exp(-4.2 * u) * std::min(1.0, double(i) / 30.0));
        out.data[i] = 0.40f * env * float(std::sin(ph) + 0.30 * std::sin(2.0 * ph));
      }
      return true;
    }
    case SfxSoundId::kUiWristbandUnlock: {
      const size_t n = size_t(0.42 * double(rate));
      out.data.resize(n);
      double ph = 0.0;
      for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / double(n);
        const double f = u < 0.33 ? 523.25 : (u < 0.66 ? 659.25 : 1046.5);
        ph += kTwoPi * f / double(rate);
        const float env = float(std::exp(-2.8 * u) * std::min(1.0, double(i) / 30.0));
        out.data[i] = 0.44f * env * float(std::sin(ph) + 0.35 * std::sin(1.5 * ph));
      }
      return true;
    }
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
    case SfxSoundId::kOtherCarLoop4:
    case SfxSoundId::kTrafficCarLoop: {
      const double hz = (id == SfxSoundId::kTrafficCarLoop)
                            ? 82.0
                            : (id == SfxSoundId::kOtherCarLoop4
                                   ? 112.0
                                   : (95.0 + 18.0 * double(int(id) - int(SfxSoundId::kOtherCarLoop))));
      fill_harmonic_loop(0.6, hz, 0.34f, 0.20f);
      return true;
    }
    case SfxSoundId::kOtherCarLoopHi:
    case SfxSoundId::kOtherCarLoop1Hi:
    case SfxSoundId::kOtherCarLoop2Hi:
    case SfxSoundId::kOtherCarLoop3Hi:
    case SfxSoundId::kOtherCarLoop4Hi:
    case SfxSoundId::kTrafficCarLoopHi: {
      const double hz = (id == SfxSoundId::kTrafficCarLoopHi)
                            ? 155.0
                            : (185.0 + 28.0 * double(int(id) - int(SfxSoundId::kOtherCarLoopHi)));
      fill_harmonic_loop(0.6, hz, 0.34f, 0.16f);
      return true;
    }
    default:
      return false;
  }
}

uint8_t one_shot_priority(SfxSoundId id) {
  switch (id) {
    // Tier 100: Critical race/UI/rewind cues — never stolen by ambient ticks.
    case SfxSoundId::kCountdown321:
    case SfxSoundId::kUiCountdownGo:
    case SfxSoundId::kUiCountdownTick:
    case SfxSoundId::kUiRaceFinish:
    case SfxSoundId::kUiLap:
    case SfxSoundId::kUiWristbandUnlock:
    case SfxSoundId::kUiRankUp:
    case SfxSoundId::kAchievementReward:
    case SfxSoundId::kRewindTapeStart:
    case SfxSoundId::kRewindTapeStop:
      return 100;
    // Tier 80: High-priority collisions, LFE punches, glass shatters, landings & UI feedback.
    case SfxSoundId::kImpactLfe:
    case SfxSoundId::kBackfireLfe:
    case SfxSoundId::kImpact3d:
    case SfxSoundId::kImpactCarOnCar:
    case SfxSoundId::kImpactDirt:
    case SfxSoundId::kImpactMetal:
    case SfxSoundId::kImpactWood:
    case SfxSoundId::kGlassShatter:
    case SfxSoundId::kGlassSideWindow:
    case SfxSoundId::kGlassRearWindow:
    case SfxSoundId::kGlassTaillight:
    case SfxSoundId::kGlassLexan:
    case SfxSoundId::kCarLandingHeavy:
    case SfxSoundId::kCarLandingLight:
    case SfxSoundId::kCarLandingDirt:
    case SfxSoundId::kUiAccept:
    case SfxSoundId::kUiCancel:
    case SfxSoundId::kUiError:
    case SfxSoundId::kUiMenuOpen:
    case SfxSoundId::kUiCheckpoint:
    case SfxSoundId::kUiSkillAward:
    case SfxSoundId::kUiSkillMulti:
    case SfxSoundId::kUiSkillBank:
    case SfxSoundId::kUiSkillBreak:
    case SfxSoundId::kUiSpeedCamera:
    case SfxSoundId::kUiPhotoShutter:
    case SfxSoundId::kUiPurchase:
    case SfxSoundId::kUiCarAdded:
    case SfxSoundId::kUiH2hGo:
    case SfxSoundId::kUiH2hLoss:
    case SfxSoundId::kSignSmash:
      return 80;
    // Tier 20: Low-priority high-rate ticks & burble chatter (first to be stolen).
    case SfxSoundId::kSurfaceHardToHard:
    case SfxSoundId::kSurfaceDirtToHard:
    case SfxSoundId::kSurfaceLeavesOn:
    case SfxSoundId::kSurfaceLeavesOff:
    case SfxSoundId::kBurblePop0:
    case SfxSoundId::kBurblePop1:
    case SfxSoundId::kBurblePop2:
    case SfxSoundId::kBurblePop3:
    case SfxSoundId::kUiMove:
      return 20;
    // Tier 50: Medium-priority mechanical/prop one-shots.
    default:
      return 50;
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
    const auto span = std::span<const uint8_t>(bank_data).subspan(size_t(s.offset), s.bytes);
    if (decode_xma2_mono(s, span, out, nullptr)) return true;
    if (ffmpeg.empty()) return false;
    const auto wave = xma2_wave(s, span);
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

SfxSoundId turbo_bov_sound_id(int level_1_to_3, int choice_1_to_5, bool hi) {
  const int lvl = std::clamp(level_1_to_3, 1, 3);
  const int ch = std::clamp(choice_1_to_5, 1, 5);
  static constexpr SfxSoundId kTable[3][5][2] = {
      {{SfxSoundId::kTurboBovLo, SfxSoundId::kTurboBovHi},
       {SfxSoundId::kTurboBovL1C2Lo, SfxSoundId::kTurboBovL1C2Hi},
       {SfxSoundId::kTurboBovL1C3Lo, SfxSoundId::kTurboBovL1C3Hi},
       {SfxSoundId::kTurboBovL1C4Lo, SfxSoundId::kTurboBovL1C4Hi},
       {SfxSoundId::kTurboBovL1C5Lo, SfxSoundId::kTurboBovL1C5Hi}},
      {{SfxSoundId::kTurboBovLoL2, SfxSoundId::kTurboBovHiL2},
       {SfxSoundId::kTurboBovL2C2Lo, SfxSoundId::kTurboBovL2C2Hi},
       {SfxSoundId::kTurboBovL2C3Lo, SfxSoundId::kTurboBovL2C3Hi},
       {SfxSoundId::kTurboBovL2C4Lo, SfxSoundId::kTurboBovL2C4Hi},
       {SfxSoundId::kTurboBovL2C5Lo, SfxSoundId::kTurboBovL2C5Hi}},
      {{SfxSoundId::kTurboBovLoL3, SfxSoundId::kTurboBovHiL3},
       {SfxSoundId::kTurboBovL3C2Lo, SfxSoundId::kTurboBovL3C2Hi},
       {SfxSoundId::kTurboBovL3C3Lo, SfxSoundId::kTurboBovL3C3Hi},
       {SfxSoundId::kTurboBovL3C4Lo, SfxSoundId::kTurboBovL3C4Hi},
       {SfxSoundId::kTurboBovL3C5Lo, SfxSoundId::kTurboBovL3C5Hi}},
  };
  return kTable[lvl - 1][ch - 1][hi ? 1 : 0];
}

SfxSoundId smash_material_sound_id(SmashMaterial mat, uint8_t surface_hint) {
  SmashMaterial effective = mat;
  if (effective == SmashMaterial::kDefault) {
    if (surface_hint == 2) effective = SmashMaterial::kFoliage;
    else if (surface_hint == 3) effective = SmashMaterial::kMetal;
    else if (surface_hint == 4) effective = SmashMaterial::kWood;
  }
  switch (effective) {
    case SmashMaterial::kWood: return SfxSoundId::kBreakWood;
    case SmashMaterial::kMetal: return SfxSoundId::kBreakMetal;
    case SmashMaterial::kGlass: return SfxSoundId::kBreakGlass;
    case SmashMaterial::kPlastic: return SfxSoundId::kBreakPlastic;
    case SmashMaterial::kFoliage: return SfxSoundId::kBreakFoliage;
    case SmashMaterial::kStone: return SfxSoundId::kBreakStone;
    case SmashMaterial::kFence: return SfxSoundId::kBreakFence;
    case SmashMaterial::kDefault:
    case SmashMaterial::kCount:
    default: return SfxSoundId::kBreakProp;
  }
}

SfxEngine::SfxEngine(int out_rate) : rate_(std::max(8000, out_rate)) {
  smooth_ = float(1.0 - std::exp(-1.0 / (0.015 * double(rate_))));
  samples_.fill(nullptr);
  other_engine_scratch_.assign(2 * 1024, 0.0f);
  event_scratch_.assign(2 * 1024, 0.0f);
}

SfxEngine::~SfxEngine() {
  stop_decoder_worker();
  drain_loaded();
  reclaim_retired();
  drain_other_engines();
  for (auto& oe : other_engines_) {
    delete oe.player;
    oe = {};
  }
  reclaim_other_car_engines();
  for (PcmSample*& p : samples_) {
    delete p;
    p = nullptr;
  }
  for (PcmSample*& p : ui_samples_) {
    delete p;
    p = nullptr;
  }
  for (PcmSample*& p : event_samples_) {
    delete p;
    p = nullptr;
  }
}

void SfxEngine::set_other_car_engine(uint32_t car_id, std::unique_ptr<EnginePlayer> player) {
  reclaim_other_car_engines();
  if (car_id == 0u) return;
  EnginePlayer* raw = player.release();
  std::lock_guard<std::mutex> lock(other_engine_mu_);
  if (!other_engine_q_.push({car_id, raw})) delete raw;
}

void SfxEngine::reclaim_other_car_engines() {
  EnginePlayer* p = nullptr;
  while (other_engine_retired_q_.pop(p)) delete p;
}

int SfxEngine::other_car_engine_count() const { return other_engine_count_.load(std::memory_order_acquire); }

void SfxEngine::retire_other_engine(EnginePlayer* p) {
  if (p && !other_engine_retired_q_.push(p)) delete p;  // the ring is full: free here (rare)
}

void SfxEngine::drain_other_engines() {
  OtherEngineMsg m;
  while (other_engine_q_.pop(m)) {
    OtherEngine* at = nullptr;
    OtherEngine* free_at = nullptr;
    for (auto& oe : other_engines_) {
      if (oe.car_id == m.car_id) at = &oe;
      else if (!free_at && oe.car_id == 0u) free_at = &oe;
    }
    if (at) {
      retire_other_engine(at->player);
      at->player = m.player;
      if (!m.player) at->car_id = 0u;
    } else if (m.player) {
      if (free_at) {
        free_at->car_id = m.car_id;
        free_at->player = m.player;
      } else {
        retire_other_engine(m.player);  // table full: the car stays without its own sound
      }
    }
  }
  int n = 0;
  for (const auto& oe : other_engines_) n += oe.player ? 1 : 0;
  other_engine_count_.store(n, std::memory_order_release);
}

EnginePlayer* SfxEngine::other_engine_for(uint32_t car_id) const {
  if (car_id == 0u) return nullptr;
  for (const auto& oe : other_engines_)
    if (oe.car_id == car_id) return oe.player;
  return nullptr;
}

void SfxEngine::mix_other_engine(PersistentOtherVoice& sl, EnginePlayer& ep, float gain, float left, float right,
                                 float* bus, int frames) {
  const float g0 = sl.eng_gain, l0 = sl.eng_left, r0 = sl.eng_right;
  const float g1 = safe_clamp_f(gain, 0.0f, 4.0f, 0.0f);
  const float l1 = safe_clamp_f(left, 0.0f, 2.0f, 0.707f), r1 = safe_clamp_f(right, 0.0f, 2.0f, 0.707f);
  const int chunk_max = int(other_engine_scratch_.size() / 2);
  for (int done = 0; done < frames;) {
    const int n = std::min(chunk_max, frames - done);
    float* tmp = other_engine_scratch_.data();
    ep.render(tmp, n);
    if (bus) {
      for (int f = 0; f < n; ++f) {
        const float t = float(done + f + 1) / float(frames);
        const float g = g0 + (g1 - g0) * t;
        // The positional player renders the same signal in both channels; their
        // mean is the point source, then placed.
        const float m = (tmp[2 * f] + tmp[2 * f + 1]) * 0.5f * g;
        if (!std::isfinite(m)) continue;
        bus[2 * (done + f)] += m * (l0 + (l1 - l0) * t);
        bus[2 * (done + f) + 1] += m * (r0 + (r1 - r0) * t);
      }
    }
    done += n;
  }
  sl.eng_gain = g1;
  sl.eng_left = l1;
  sl.eng_right = r1;
}

void SfxEngine::stop_decoder_worker() {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  stop_burble_worker_.store(true, std::memory_order_release);
  if (burble_worker_.joinable()) burble_worker_.join();
  stop_rally_worker_.store(true, std::memory_order_release);
  if (rally_worker_.joinable()) rally_worker_.join();
}

SfxSoundId SfxEngine::resolve_fallback(SfxSoundId preferred, SfxSoundId fallback) const {
  const size_t pi = size_t(preferred);
  if (pi < samples_.size() && samples_[pi] && !samples_[pi]->data.empty()) return preferred;
  return fallback;
}

void SfxEngine::reclaim_retired() {
  std::lock_guard<std::mutex> lock(reclaim_mu_);
  PcmSample* old = nullptr;
  while (retired_q_.pop(old)) {
    delete old;
  }
}

void SfxEngine::drain_queue(SpscQueue<LoadedSample>& q) {
  LoadedSample ls;
  while (q.pop(ls)) {
    if (ls.ui >= 0) {
      PcmSample* old = ls.pcm;
      if (ls.ui < kMaxUiSamples) std::swap(old, ui_samples_[size_t(ls.ui)]);
      if (old && !retired_q_.push(old)) delete old;
      continue;
    }
    if (ls.ev >= 0) {
      PcmSample* old = ls.pcm;
      if (ls.ev < kMaxEventSamples) std::swap(old, event_samples_[size_t(ls.ev)]);
      if (old && !retired_q_.push(old)) delete old;
      continue;
    }
    const size_t idx = size_t(ls.id);
    if (idx < samples_.size()) {
      PcmSample* old = samples_[idx];
      samples_[idx] = ls.pcm;
      if (old) {
        // Retire off the audio thread to avoid real-time heap frees; only fall back
        // to direct delete if the 512-slot retirement ring is completely saturated.
        if (!retired_q_.push(old)) delete old;
      }
    } else if (ls.pcm) {
      if (!retired_q_.push(ls.pcm)) delete ls.pcm;
    }
  }
}

void SfxEngine::drain_loaded() {
  drain_queue(loaded_q_);
  drain_queue(burble_loaded_q_);
  drain_queue(rally_loaded_q_);
  drain_queue(direct_loaded_q_);
}

void SfxEngine::set_sample(SfxSoundId id, PcmSample sample) {
  reclaim_retired();
  const size_t idx = size_t(id);
  if (idx >= samples_.size()) return;
  auto* p = new PcmSample(std::move(sample));
  std::lock_guard<std::mutex> lock(direct_q_mu_);
  if (!direct_loaded_q_.push({id, p})) delete p;
}

void SfxEngine::set_ui_sample(int sample_slot, PcmSample sample) {
  reclaim_retired();
  if (sample_slot < 0 || sample_slot >= kMaxUiSamples) return;
  auto* p = new PcmSample(std::move(sample));
  std::lock_guard<std::mutex> lock(direct_q_mu_);
  if (!direct_loaded_q_.push({SfxSoundId::kUiAccept, p, sample_slot})) delete p;
}

void SfxEngine::set_event_sample(int slot, PcmSample sample) {
  reclaim_retired();
  if (slot < 0 || slot >= kMaxEventSamples) return;
  auto* p = new PcmSample(std::move(sample));
  std::lock_guard<std::mutex> lock(direct_q_mu_);
  if (!direct_loaded_q_.push({SfxSoundId::kWind, p, -1, slot})) delete p;
}

int SfxEngine::tyre_event_count() const {
  int n = 0;
  for (const auto& g : tyre_events_)
    for (const auto& w : g)
      for (const auto& e : w)
        if (e.ok) ++n;
  return n;
}

int SfxEngine::loaded_ui_sample_count() const {
  int n = 0;
  for (const PcmSample* p : ui_samples_)
    if (p && !p->data.empty()) ++n;
  return n;
}

bool SfxEngine::is_ui_sample_active(int sample) const {
  for (const auto& o : one_shots_)
    if (o.active && (o.ui == sample || (o.stealing && o.next_ui == sample))) return true;
  return false;
}

void SfxEngine::populate_procedural_fallbacks() {
  procedural_mode_ = true;
  for (int i = 0; i < int(SfxSoundId::kCount); ++i) {
    const auto id = SfxSoundId(i);
    PcmSample pcm;
    if (synthesize_fallback_sfx(id, 24000, pcm) && !pcm.data.empty()) {
      set_sample(id, std::move(pcm));
    }
  }
}

bool SfxEngine::decoding() const { return decoders_running_.load(std::memory_order_acquire) > 0; }

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

bool SfxEngine::is_one_shot_active(SfxSoundId id) const {
  for (const auto& o : one_shots_) {
    if (!o.active) continue;
    if (o.id == id || (o.stealing && o.next_id == id)) return true;
  }
  return false;
}

uint32_t SfxEngine::bound_other_car_id(int voice_slot) const {
  if (voice_slot < 0 || voice_slot >= VehicleAudioState::kMaxOtherCars) return 0u;
  return other_slots_[size_t(voice_slot)].bound_car_id;
}

bool SfxEngine::load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                     const std::string& burble_bank, std::string* note,
                     const std::filesystem::path& state_root) {
  stop_decoder_worker();
  reclaim_retired();
  if (game_root.empty()) return false;
  const auto audio_dir = game_root / "media" / "audio";

  tires_cfg_.load(game_root, nullptr, state_root);
  coll_cfg_.load(game_root, nullptr, state_root);
  ui_map_.load(game_root, nullptr);
  ui_catalog_.reset();
  backfire_cfg_.load(game_root, nullptr);
  car_rolloff_.load(game_root, false, false, nullptr);
  traffic_rolloff_.load(game_root, true, false, nullptr);

  std::vector<DecodeJob> jobs;
  std::vector<DecodeJob> ui_jobs;  // decoded after the driving sounds
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
                 {SfxSoundId::kSurfaceDirtToHard, "Tires/Tires/SurfaceChanges/DirtToHard"},
                 {SfxSoundId::kSurfaceLeavesOn, "Tires/Tires/SurfaceChanges/Leaves_On"},
                 {SfxSoundId::kSurfaceLeavesOff, "Tires/Tires/SurfaceChanges/Leaves_Off"}});
  add_batch_fev("cars/Transmissions/Transmissions.fev",
                {{SfxSoundId::kTransWhine, "Transmissions/Transmissions/L0"},
                 {SfxSoundId::kTransWhineL1, "Transmissions/Transmissions/L1"},
                 {SfxSoundId::kTransWhineL2, "Transmissions/Transmissions/L2"},
                 {SfxSoundId::kTransWhineL3, "Transmissions/Transmissions/L3"},
                 {SfxSoundId::kShiftStart, "Transmissions/Transmissions/ShiftStart/L0"},
                 {SfxSoundId::kShiftStartL1, "Transmissions/Transmissions/ShiftStart/L1"},
                 {SfxSoundId::kShiftStartL2, "Transmissions/Transmissions/ShiftStart/L2"},
                 {SfxSoundId::kShiftStartL3, "Transmissions/Transmissions/ShiftStart/L3"},
                 {SfxSoundId::kShiftComplete, "Transmissions/Transmissions/ShiftComplete/L0"},
                 {SfxSoundId::kShiftCompleteL1, "Transmissions/Transmissions/ShiftComplete/L1"},
                 {SfxSoundId::kShiftCompleteL2, "Transmissions/Transmissions/ShiftComplete/L2"},
                 {SfxSoundId::kShiftCompleteL3, "Transmissions/Transmissions/ShiftComplete/L3"}});
  {
    FevProject turbo_fev;
    if (load_fev(audio_dir / "cars/Turbos/Turbos.fev", turbo_fev, nullptr)) {
      auto add_turbo = [&](SfxSoundId id, std::string_view ev_path) {
        std::filesystem::path bank;
        uint32_t sub = 0;
        if (resolve_event_sample(turbo_fev, ev_path, bank, sub)) jobs.push_back({id, bank, sub});
      };
      add_turbo(SfxSoundId::kTurboSpool, "Turbos/Turbos/L1 Group/L1");
      add_turbo(SfxSoundId::kTurboSpoolL2, "Turbos/Turbos/L2 Group/L2");
      add_turbo(SfxSoundId::kTurboSpoolL3, "Turbos/Turbos/L3 Group/L3");
      // Load all per-car BOV choices 1..5 across L1..L3 Lo/Hi (honoring blowoffL1..L3Choice).
      for (int lvl = 1; lvl <= 3; ++lvl) {
        const std::string grp = "Turbos/Turbos/L" + std::to_string(lvl) + " Group/L" + std::to_string(lvl) + "_BlowOff_";
        for (int ch = 1; ch <= 5; ++ch) {
          const std::string pfx = grp + std::to_string(ch);
          add_turbo(turbo_bov_sound_id(lvl, ch, false), pfx + "_Lo");
          add_turbo(turbo_bov_sound_id(lvl, ch, true), pfx + "_Hi");
        }
      }
    }
  }
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
                 {SfxSoundId::kEngineLfeLoop, "EngineLFE/EngineLFE/EngineLFE"},
                 {SfxSoundId::kEngineLfe4, "EngineLFE/EngineLFE/EngineLFE_4cyl"},
                 {SfxSoundId::kEngineLfe6, "EngineLFE/EngineLFE/EngineLFE_6cyl"},
                 {SfxSoundId::kEngineLfe8, "EngineLFE/EngineLFE/EngineLFE_8cyl"},
                 {SfxSoundId::kEngineLfe10, "EngineLFE/EngineLFE/EngineLFE_10cyl"},
                 {SfxSoundId::kEngineLfe12, "EngineLFE/EngineLFE/EngineLFE_12cyl"}});
  // Additional car subsystem FEV projects: ElectricEngine.fev (multi-band harmonic layers),
  // ExhNoise.fev (high-RPM exhaust air-rush), Damage.fev (mechanical engine knock/rattle/misfire),
  // Reflections.fev (canyon/guardrail slap-back), and ActiveAero.fev (deploying wing/airbrake).
  {
    auto add_first_fev_or_fsb = [&](const char* fev_rel, SfxSoundId id) -> bool {
      const auto fev_path = cars::mods::resolve(audio_dir / fev_rel);
      FevProject proj;
      if (load_fev(fev_path, proj, nullptr)) {
        for (const auto& ev : proj.events) {
          std::filesystem::path bank;
          uint32_t sub = 0;
          if (resolve_event_sample(proj, ev.path, bank, sub)) {
            jobs.push_back({id, bank, sub});
            return true;
          }
        }
      }
      // Directory FSB4 fallback when FEV references a sibling soundbank.
      std::error_code ec;
      const auto dir = fev_path.parent_path();
      if (std::filesystem::is_directory(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
          if (entry.is_regular_file(ec) && lower_str(entry.path().extension().string()) == ".fsb") {
            jobs.push_back({id, entry.path(), 0});
            return true;
          }
        }
      }
      return false;
    };

    // Multi-band ElectricEngine.fev: resolve up to 3 distinct harmonic pitch layers (low/mid/high).
    {
      static constexpr SfxSoundId kEvBands[3] = {SfxSoundId::kElectricMotor, SfxSoundId::kElectricMotorMid,
                                                 SfxSoundId::kElectricMotorHigh};
      const auto fev_path = cars::mods::resolve(audio_dir / "cars/ElectricMotor/ElectricEngine.fev");
      std::vector<std::pair<std::filesystem::path, uint32_t>> ev_samples;
      auto push_unique = [&](const std::filesystem::path& bank, uint32_t sub) {
        if (ev_samples.size() >= 3) return;
        for (const auto& p : ev_samples)
          if (p.first == bank && p.second == sub) return;
        ev_samples.emplace_back(bank, sub);
      };
      FevProject proj;
      if (load_fev(fev_path, proj, nullptr)) {
        for (const auto& sd : proj.sound_defs) {
          for (const auto& en : sd.entries) {
            if (en.entry_type == 0 && !en.wavebank_name.empty()) {
              const auto bp = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
              std::error_code ec;
              if (std::filesystem::exists(bp, ec)) push_unique(bp, en.subsound_index);
            }
          }
        }
      }
      if (ev_samples.empty()) {
        std::error_code ec;
        const auto dir = fev_path.parent_path();
        if (std::filesystem::is_directory(dir, ec)) {
          for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && lower_str(entry.path().extension().string()) == ".fsb") {
              std::vector<uint8_t> raw;
              std::vector<FsbSample> fsb_s;
              const uint32_t n = (read_bytes(entry.path(), raw) && read_fsb4(raw, fsb_s, nullptr))
                                     ? std::min<uint32_t>(3u, uint32_t(fsb_s.size()))
                                     : 1u;
              for (uint32_t bi = 0; bi < n; ++bi) push_unique(entry.path(), bi);
              break;
            }
          }
        }
      }
      for (size_t i = 0; i < ev_samples.size() && i < 3; ++i) {
        jobs.push_back({kEvBands[i], ev_samples[i].first, ev_samples[i].second});
      }
    }

    // ExhNoise.fev: high-RPM exhaust gas/air-rush layer.
    if (!add_first_fev_or_fsb("cars/ExhNoise/ExhNoise.fev", SfxSoundId::kExhNoise)) {
      add_first_fev_or_fsb("cars/ExhaustNoise/ExhNoise.fev", SfxSoundId::kExhNoise);
    }

    // Damage.fev: mechanical engine damage rattle/knock loop + misfire sputter (disc path: cars/Engines/Damage/Damage.fev).
    {
      std::error_code dmg_ec;
      auto dmg_fev = cars::mods::resolve(audio_dir / "cars/Engines/Damage/Damage.fev");
      if (!std::filesystem::exists(dmg_fev, dmg_ec)) {
        const auto alt_fev = cars::mods::resolve(audio_dir / "cars/Damage/Damage.fev");
        if (std::filesystem::exists(alt_fev, dmg_ec)) dmg_fev = alt_fev;
      }
      // The disc's one event (Damage/Damage/DamageEngine) has one layer whose
      // instances (DamSlow, DamFast) crossfade over NormRPM; its volume
      // envelopes on Damage, NormRPM and Throttle set the level
      // (SPEC-AUDIO-EVENTS "Other vehicle event projects").
      damage_event_ = FevEvent{};
      damage_event_ok_ = false;
      FevProject proj;
      if (load_fev(dmg_fev, proj, nullptr)) {
        for (const auto& ev : proj.events) {
          if (!ev.is_complex() || ev.layers.empty()) continue;
          static constexpr SfxSoundId kDamageIds[kDamageInstances] = {SfxSoundId::kEngineDamageLoop,
                                                                      SfxSoundId::kEngineDamageFast};
          damage_sound_ids_.fill(SfxSoundId::kCount);
          int slot = 0;
          for (const auto& si : ev.layers.front().instances) {
            if (slot >= kDamageInstances || si.sound_def_index >= proj.sound_defs.size()) break;
            for (const auto& en : proj.sound_defs[si.sound_def_index].entries) {
              if (en.entry_type != 0 || en.wavebank_name.empty()) continue;
              jobs.push_back({kDamageIds[slot], cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb")),
                              en.subsound_index});
              damage_sound_ids_[size_t(slot)] = kDamageIds[slot];
              break;
            }
            ++slot;
          }
          damage_event_ = ev;
          damage_event_.layers.resize(1);
          damage_param_damage_ = ev.find_param("Damage");
          damage_param_rpm_ = ev.find_param("NormRPM");
          damage_param_throttle_ = ev.find_param("Throttle");
          damage_event_ok_ = slot > 0;
          break;
        }
      }
      // Without the event there are no level curves: the damage loop stays
      // silent rather than playing a bare bank at invented gains.
    }

    // Tires.fev LOD1 and Wind.fev: the events themselves, every layer instance a
    // loop voice playing its sound definition's first wavetable entry.
    {
      std::vector<std::pair<std::filesystem::path, uint32_t>> ev_slots;
      auto slot_of = [&](const std::filesystem::path& bank, uint32_t sub) -> int {
        for (size_t i = 0; i < ev_slots.size(); ++i)
          if (ev_slots[i].first == bank && ev_slots[i].second == sub) return int(i);
        if (ev_slots.size() >= size_t(kMaxEventSamples)) return -1;
        ev_slots.emplace_back(bank, sub);
        DecodeJob j;
        j.bank_path = bank;
        j.subsound = sub;
        j.ev = int(ev_slots.size() - 1);
        jobs.push_back(j);
        return j.ev;
      };
      static constexpr const char* kParamNames[int(EvParam::kCount)] = {
          "SlipAngle", "SlipRatio", "SurfaceIndex", "TireRPM", "Load",
          "CombinedSlip", "Speed", "TopSpeedPercent", "BoostCarWindAudio", "MusicActive"};
      auto bind = [&](EventLoop& el, const FevProject& proj, const FevEvent* ev) {
        el = EventLoop{};
        if (!ev || !ev->is_complex()) return;
        el.ev = *ev;
        for (int p = 0; p < int(EvParam::kCount); ++p) el.param[size_t(p)] = int8_t(ev->find_param(kParamNames[p]));
        el.slot.fill(-1);
        el.tpl_gain.fill(1.0f);
        el.tpl_pitch.fill(1.0f);
        int n = 0;
        for (const auto& ly : ev->layers) {
          for (const auto& si : ly.instances) {
            if (n >= kMaxEventVoices) break;
            if (si.sound_def_index < proj.sound_defs.size()) {
              const auto& sd = proj.sound_defs[si.sound_def_index];
              const auto* t = proj.template_of(sd);
              if (t) {
                el.tpl_gain[size_t(n)] = t->volume;
                el.tpl_pitch[size_t(n)] = std::exp2(t->pitch * kFevPitchOctaves);
              }
              // Spawned or randomly picked one-shots (OffRoad's Stones_Grains /
              // Stones_OneShots, play mode 6) are not loops: left out (Q331).
              const bool one_shots = t && (t->spawned() || (t->play_mode == 6 && sd.entries.size() > 1));
              for (const auto& en : sd.entries) {
                if (one_shots) break;
                if (en.entry_type != 0 || en.wavebank_name.empty()) continue;
                el.slot[size_t(n)] =
                    int16_t(slot_of(cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb")), en.subsound_index));
                break;
              }
            }
            ++n;
          }
        }
        el.count = n;
        el.ok = n > 0;
        if (!ev->layers.empty()) {
          const auto& envs = ev->layers.front().envelopes;
          for (size_t e = 0; e < envs.size(); ++e)
            if (envs[e].parent_index < 0 && envs[e].dsp_param_index == 0 && lower_str(envs[e].dsp_name) == "fmod highpass")
              el.hp_env = int(e);
        }
      };
      for (auto& g : tyre_events_)
        for (auto& w : g)
          for (auto& e : w) e = EventLoop{};
      wind_event_ = EventLoop{};
      FevProject tires;
      if (load_fev(cars::mods::resolve(audio_dir / "cars/Tires/Tires.fev"), tires, nullptr)) {
        static constexpr const char* kGroups[kTyreGroups] = {"OnRoad", "OffRoad", "Grass", "Brick", "Trackway"};
        static constexpr const char* kWheels[4] = {"LF", "RF", "LR", "RR"};
        static constexpr const char* kKinds[2] = {"Roll", "Skid"};
        for (int g = 0; g < kTyreGroups; ++g)
          for (int w = 0; w < 4; ++w)
            for (int k = 0; k < 2; ++k) {
              const std::string path =
                  std::string("Tires/Tires/LOD1/") + kGroups[g] + "/" + kKinds[k] + "_" + kWheels[w];
              bind(tyre_events_[size_t(g)][size_t(w)][size_t(k)], tires, tires.find_event(path));
            }
      }
      FevProject wind;
      if (load_fev(cars::mods::resolve(audio_dir / "cars/Wind/Wind.fev"), wind, nullptr))
        bind(wind_event_, wind, wind.find_event("Wind/Wind/Wind"));
    }

    add_first_fev_or_fsb("cars/Reflections/Reflections.fev", SfxSoundId::kRoadsideReflection);
    add_first_fev_or_fsb("cars/ActiveAero/ActiveAero.fev", SfxSoundId::kActiveAero);
  }
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
      auto add_coll_pair = [&](SfxSoundId imp_id, SfxSoundId scrape_id, std::string_view s1, std::string_view s2) {
        if (const auto* cp = coll_cfg_.find_pair(s1, s2)) {
          if (!cp->impact3d.empty() && resolve_event_sample(coll_fev, cp->impact3d, bank, sub)) {
            jobs.push_back({imp_id, bank, sub});
          }
          if (scrape_id != SfxSoundId::kCount && !cp->scrape3d.empty() &&
              resolve_event_sample(coll_fev, cp->scrape3d, bank, sub)) {
            jobs.push_back({scrape_id, bank, sub});
          }
        }
      };
      add_coll_pair(SfxSoundId::kImpactCarOnCar, SfxSoundId::kCount, "CarAluminum", "CarAluminum");
      add_coll_pair(SfxSoundId::kImpactDirt, SfxSoundId::kScrapeDirt, "Dirt", "CarAluminum");
      add_coll_pair(SfxSoundId::kImpactMetal, SfxSoundId::kScrapeMetal, "Metal", "CarAluminum");
      add_coll_pair(SfxSoundId::kImpactWood, SfxSoundId::kScrapeWood, "Wood", "CarAluminum");
      if (pair && !pair->scrape3d.empty() && resolve_event_sample(coll_fev, pair->scrape3d, bank, sub)) {
        jobs.push_back({SfxSoundId::kScrapeLoop, bank, sub});
      }
      // BreakEvents for sign/prop smashes (generic + per-material smashables),
      // plus Scrape3D / Glass / Whoosh3D fallback search.
      bool has_break = false, has_scrape = false, has_glass = false, has_whoosh = false;
      std::array<bool, int(SmashMaterial::kCount)> has_mat_break{};
      for (const auto& j : jobs)
        if (j.id == SfxSoundId::kScrapeLoop) has_scrape = true;
      auto classify_smash_mat = [](const std::string& s) -> SmashMaterial {
        if (s.find("glass") != std::string::npos || s.find("window") != std::string::npos) return SmashMaterial::kGlass;
        if (s.find("fence") != std::string::npos || s.find("gate") != std::string::npos ||
            s.find("chainlink") != std::string::npos)
          return SmashMaterial::kFence;
        if (s.find("plastic") != std::string::npos || s.find("cone") != std::string::npos ||
            s.find("bollard") != std::string::npos || s.find("rubber") != std::string::npos)
          return SmashMaterial::kPlastic;
        if (s.find("bush") != std::string::npos || s.find("foliage") != std::string::npos ||
            s.find("shrub") != std::string::npos || s.find("cactus") != std::string::npos ||
            s.find("hedge") != std::string::npos || s.find("tree") != std::string::npos ||
            s.find("plant") != std::string::npos)
          return SmashMaterial::kFoliage;
        if (s.find("stone") != std::string::npos || s.find("rock") != std::string::npos ||
            s.find("brick") != std::string::npos || s.find("concrete") != std::string::npos ||
            s.find("pot") != std::string::npos)
          return SmashMaterial::kStone;
        if (s.find("metal") != std::string::npos || s.find("sign") != std::string::npos ||
            s.find("pole") != std::string::npos || s.find("steel") != std::string::npos ||
            s.find("bin") != std::string::npos)
          return SmashMaterial::kMetal;
        if (s.find("wood") != std::string::npos || s.find("crate") != std::string::npos ||
            s.find("post") != std::string::npos || s.find("barrel") != std::string::npos ||
            s.find("hay") != std::string::npos)
          return SmashMaterial::kWood;
        return SmashMaterial::kDefault;
      };
      for (const auto& p : coll_cfg_.pairs) {
        if (!p.is_break || p.impact3d.empty()) continue;
        if (!resolve_event_sample(coll_fev, p.impact3d, bank, sub)) continue;
        if (!has_break) {
          jobs.push_back({SfxSoundId::kBreakProp, bank, sub});
          has_break = true;
        }
        const std::string tag = lower_str(p.surface1 + " " + p.surface2 + " " + p.impact3d);
        const SmashMaterial mat = classify_smash_mat(tag);
        if (mat != SmashMaterial::kDefault && !has_mat_break[size_t(mat)]) {
          has_mat_break[size_t(mat)] = true;
          jobs.push_back({smash_material_sound_id(mat), bank, sub});
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
        } else if (lp.find("break") != std::string::npos || lp.find("smash") != std::string::npos ||
                   lp.find("prop") != std::string::npos) {
          const SmashMaterial mat = classify_smash_mat(lp);
          if (mat != SmashMaterial::kDefault && !has_mat_break[size_t(mat)] &&
              resolve_event_sample(coll_fev, ev.path, bank, sub)) {
            has_mat_break[size_t(mat)] = true;
            jobs.push_back({smash_material_sound_id(mat), bank, sub});
          }
        }
      }
    }
    // Dedicated Glass.fev for all 5 glass fracture & shatter variants (WINDSHIELD_SMASH,
    // SIDEWINDOW_SHATTER, REARWINDOW_SHATTER, TAILLIGHT_SHATTER, LEXAN_SMASH).
    {
      FevProject glass_fev;
      if (load_fev(audio_dir / "collisions/Glass.fev", glass_fev, nullptr)) {
        bool have_windshield = false, have_side = false, have_rear = false, have_tail = false, have_lexan = false;
        std::filesystem::path first_bank;
        uint32_t first_sub = 0;
        bool have_first = false;
        for (const auto& ev : glass_fev.events) {
          std::filesystem::path bank;
          uint32_t sub = 0;
          if (!resolve_event_sample(glass_fev, ev.path, bank, sub)) continue;
          if (!have_first) {
            first_bank = bank;
            first_sub = sub;
            have_first = true;
          }
          const std::string lp = lower_str(ev.path);
          if (!have_windshield && lp.find("windshield") != std::string::npos) {
            jobs.push_back({SfxSoundId::kGlassShatter, bank, sub});
            have_windshield = true;
          } else if (!have_side && (lp.find("sidewindow") != std::string::npos || lp.find("side_window") != std::string::npos)) {
            jobs.push_back({SfxSoundId::kGlassSideWindow, bank, sub});
            have_side = true;
          } else if (!have_rear && (lp.find("rearwindow") != std::string::npos || lp.find("rear_window") != std::string::npos)) {
            jobs.push_back({SfxSoundId::kGlassRearWindow, bank, sub});
            have_rear = true;
          } else if (!have_tail && (lp.find("taillight") != std::string::npos || lp.find("tail_light") != std::string::npos)) {
            jobs.push_back({SfxSoundId::kGlassTaillight, bank, sub});
            have_tail = true;
          } else if (!have_lexan && lp.find("lexan") != std::string::npos) {
            jobs.push_back({SfxSoundId::kGlassLexan, bank, sub});
            have_lexan = true;
          }
        }
        if (!have_windshield && have_first) {
          jobs.push_back({SfxSoundId::kGlassShatter, first_bank, first_sub});
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
      // Every ui4audio event by its own sample and level (SPEC-AUDIO-EVENTS):
      // the banked events all decode; of the streamed bank only the events
      // the engine itself fires, so the 46 MB stream bank is not decoded whole.
      {
        FevProject gen_fev;
        std::vector<const FevProject*> projects{&ui_fev};
        if (load_fev(audio_dir / "gamemodes/General.fev", gen_fev, nullptr)) projects.push_back(&gen_fev);
        auto cat = std::make_shared<UiAudioCatalog>();
        cat->build(ui_map_, projects);
        const auto& evs = cat->events();
        std::vector<char> fired(evs.size(), 0);
        std::vector<std::string> scenes{std::string{}};
        for (const auto& [scene, block] : ui_map_.scene_overrides) scenes.push_back(scene);
        for (const auto& name : engine_ui_play_names())
          for (const auto& scene : scenes)
            if (const int at = cat->resolve(name, scene); at >= 0) fired[size_t(at)] = 1;
        // The cutscenes' stings (CCutsceneAudioTrigger) fire too.
        for (size_t i = 0; i < evs.size(); ++i)
          if (is_cutscene_ui_event(evs[i].path)) fired[i] = 1;
        for (size_t i = 0; i < evs.size(); ++i) {
          for (const auto& l : evs[i].layers) {
            if (l.sample < 0 || l.sample >= kMaxUiSamples) continue;
            const bool streamed = lower_str(l.bank.stem().string()).find("_streams") != std::string::npos;
            if (!streamed || fired[i]) ui_jobs.push_back({SfxSoundId::kUiAccept, l.bank, l.subsound, l.sample});
          }
        }
        ui_catalog_ = std::move(cat);
      }
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
      add_ui(SfxSoundId::kUiPause, "Pause", "UIInGame/UI/Nav/HUD_Pause");
      add_ui(SfxSoundId::kUiUnPause, "UnPause", "UIInGame/UI/Nav/HUD_UnPause");
      add_ui(SfxSoundId::kUiVscroll, "Vscroll", "UIInGame/UI/Nav/Move");
      add_ui(SfxSoundId::kRewindTapeStart, "Skip", "UIInGame/UI/Mulligan/TapeStart");
      add_ui(SfxSoundId::kRewindLoop, "Rewind", "UIInGame/UI/Mulligan/TapeLoop");
      add_ui(SfxSoundId::kRewindTapeStop, "RewindStop", "UIInGame/UI/Mulligan/TapeStop");
      add_ui(SfxSoundId::kSignSmash, "Hud_Collection", "UIInGame/UI/HorizonHud/COLLECTION");
      add_ui(SfxSoundId::kUiSkillAward, "Hud_SkillAward", "UIInGame/UI/HorizonHud/SKILL_AWARD");
      add_ui(SfxSoundId::kAchievementReward, "HUD_General_Notification_Reward",
             "UIInGame/UI/HorizonHud/HUD_General_Notification_Reward");
      add_ui(SfxSoundId::kUiSpeedCamera, "SpeedCamera", "UIInGame/UI/HorizonHud/SPEED_CAMERA");
      add_ui(SfxSoundId::kUiLap, "Hud_Lap", "UIInGame/UI/HorizonHud/LAP");
      add_ui(SfxSoundId::kUiWristbandUnlock, "WristbandUnlock", "UIInGame/UI/HorizonHud/WRISTBAND_UNLOCK");
      // General.fev -> General/SpeedCamera fallback when not in UIInGame.fev.
      {
        bool have_sc = false;
        for (const auto& j : jobs)
          if (j.id == SfxSoundId::kUiSpeedCamera) have_sc = true;
        if (!have_sc) {
          FevProject gen_fev;
          if (load_fev(audio_dir / "gamemodes/General.fev", gen_fev, nullptr)) {
            for (const auto& ev : gen_fev.events) {
              std::filesystem::path bank;
              uint32_t sub = 0;
              if (lower_str(ev.path).find("speedcamera") != std::string::npos &&
                  resolve_event_sample(gen_fev, ev.path, bank, sub)) {
                jobs.push_back({SfxSoundId::kUiSpeedCamera, bank, sub});
                break;
              }
            }
          }
        }
      }
      // Direct UIInGame.fsb lookup for core FH1 driving, skill, race, shop & photo stings.
      {
        const auto ui_fsb = cars::mods::resolve(audio_dir / "UI/UIInGame/UIInGame.fsb");
        std::vector<uint8_t> raw;
        std::vector<FsbSample> fsb_s;
        if (read_bytes(ui_fsb, raw) && read_fsb4(raw, fsb_s, nullptr)) {
          auto has_ui_job = [&](SfxSoundId id) {
            for (const auto& j : jobs)
              if (j.id == id) return true;
            return false;
          };
          auto add_fsb_match = [&](SfxSoundId id, std::string_view needle) {
            if (has_ui_job(id)) return;
            const std::string want = lower_str(std::string(needle));
            for (uint32_t si = 0; si < fsb_s.size(); ++si) {
              if (lower_str(fsb_s[si].name).find(want) != std::string::npos) {
                jobs.push_back({id, ui_fsb, si});
                return;
              }
            }
          };
          add_fsb_match(SfxSoundId::kRewindTapeStop, "tapestop");
          add_fsb_match(SfxSoundId::kUiCountdownTick, "countdown_tick");
          add_fsb_match(SfxSoundId::kUiCountdownTick, "countdowntick");
          add_fsb_match(SfxSoundId::kUiCountdownTick, "countdown_beep");
          add_fsb_match(SfxSoundId::kUiCountdownTick, "countdown_321");
          add_fsb_match(SfxSoundId::kUiCountdownGo, "countdowngo");
          add_fsb_match(SfxSoundId::kUiCheckpoint, "hud_checkpointpassed");
          add_fsb_match(SfxSoundId::kUiLap, "hud_lap");
          add_fsb_match(SfxSoundId::kUiLap, "lapcomplete");
          add_fsb_match(SfxSoundId::kUiWristbandUnlock, "wristband");
          add_fsb_match(SfxSoundId::kUiRaceFinish, "racefinishstreet_st");
          add_fsb_match(SfxSoundId::kUiSkillMulti, "driving_skills_multi_slam");
          add_fsb_match(SfxSoundId::kUiSkillBank, "ds_bankpoints");
          add_fsb_match(SfxSoundId::kUiSkillBreak, "missionfail");
          add_fsb_match(SfxSoundId::kUiRankUp, "skills_tracker_level_up");
          add_fsb_match(SfxSoundId::kUiPhotoShutter, "ui_camera_photo");
          add_fsb_match(SfxSoundId::kUiPurchase, "hud_buy");
          add_fsb_match(SfxSoundId::kUiCarAdded, "caraddedtogarage");
          add_fsb_match(SfxSoundId::kUiH2hGo, "hud_raceencounter_go");
          add_fsb_match(SfxSoundId::kUiH2hLoss, "hud_raceencounter_lost");
        }
      }
      // Hub / Homespace ambience beds (AMB_FA_Autoshow, AMB_FA_Paintshop, AMB_FA_Workshop).
      {
        auto add_hub_amb = [&](SfxSoundId id, const char* rel_fsb, const char* prefer_substr) {
          const auto p = cars::mods::resolve(audio_dir / rel_fsb);
          std::vector<uint8_t> raw;
          std::vector<FsbSample> fsb_s;
          if (!read_bytes(p, raw) || !read_fsb4(raw, fsb_s, nullptr) || fsb_s.empty()) return;
          uint32_t pick = 0;
          const std::string want = lower_str(prefer_substr);
          for (uint32_t si = 0; si < fsb_s.size(); ++si) {
            if (lower_str(fsb_s[si].name).find(want) != std::string::npos) {
              pick = si;
              break;
            }
          }
          jobs.push_back({id, p, pick});
        };
        add_hub_amb(SfxSoundId::kHubAmbAutoshow, "tracks/uiautoshow/AMB_FA_Autoshow.fsb", "cheers");
        add_hub_amb(SfxSoundId::kHubAmbPaintshop, "tracks/uipaintshop/AMB_FA_Paintshop.fsb", "ambience");
        add_hub_amb(SfxSoundId::kHubAmbWorkshop, "tracks/uiworkshop/AMB_FA_Workshop.fsb", "ambience");
      }
      // Race 3-2-1 countdown, HorizonCountdowns tick, & Mulligan rewind loop/stop discovery.
      bool has_countdown = false, has_countdown_tick = false, has_rew_loop = false, has_rew_stop = false;
      for (const auto& j : jobs) {
        if (j.id == SfxSoundId::kCountdown321) has_countdown = true;
        if (j.id == SfxSoundId::kUiCountdownTick) has_countdown_tick = true;
        if (j.id == SfxSoundId::kRewindLoop) has_rew_loop = true;
        if (j.id == SfxSoundId::kRewindTapeStop) has_rew_stop = true;
      }
      for (const auto& ev : ui_fev.events) {
        const std::string lp = lower_str(ev.path);
        std::filesystem::path bank;
        uint32_t sub = 0;
        if (lp.find("horizoncountdowns/") != std::string::npos) {
          const bool looks_tick = (lp.find("tick") != std::string::npos || lp.find("beep") != std::string::npos ||
                                   lp.find("blip") != std::string::npos);
          const auto multi = resolve_event_samples(ui_fev, ev.path, 4);
          if (!multi.empty()) {
            if (looks_tick && !has_countdown_tick) {
              jobs.push_back({SfxSoundId::kUiCountdownTick, multi[0].first, multi[0].second});
              has_countdown_tick = true;
            } else if (!has_countdown) {
              jobs.push_back({SfxSoundId::kCountdown321, multi[0].first, multi[0].second});
              has_countdown = true;
              if (!has_countdown_tick && multi.size() > 1) {
                jobs.push_back({SfxSoundId::kUiCountdownTick, multi[1].first, multi[1].second});
                has_countdown_tick = true;
              }
            } else if (!has_countdown_tick) {
              jobs.push_back({SfxSoundId::kUiCountdownTick, multi[0].first, multi[0].second});
              has_countdown_tick = true;
            }
          }
        } else if (!has_rew_stop && lp.find("mulligan") != std::string::npos &&
                   lp.find("tapestop") != std::string::npos && resolve_event_sample(ui_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kRewindTapeStop, bank, sub});
          has_rew_stop = true;
        } else if (!has_rew_loop && lp.find("mulligan") != std::string::npos &&
                   lp.find("tapestart") == std::string::npos && lp.find("tapestop") == std::string::npos &&
                   resolve_event_sample(ui_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kRewindLoop, bank, sub});
          has_rew_loop = true;
        }
      }
      if (!has_countdown_tick && has_countdown) {
        for (const auto& j : jobs) {
          if (j.id == SfxSoundId::kCountdown321) {
            jobs.push_back({SfxSoundId::kUiCountdownTick, j.bank_path, j.subsound});
            break;
          }
        }
      }
    }
  }

  // Exhaust burble & backfire bank: partition overrun <Burble> crackles vs shift/decel <Backfire> pops.
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
      const uint32_t total =
          (read_bytes(bpath, raw) && read_fsb4(raw, fsb_s, nullptr)) ? uint32_t(fsb_s.size()) : 1u;
      std::vector<uint32_t> burble_subs;
      std::vector<uint32_t> backfire_subs;
      for (uint32_t si = 0; si < fsb_s.size(); ++si) {
        const std::string nm = lower_str(fsb_s[si].name);
        if (nm.find("burble") != std::string::npos || nm.find("overrun") != std::string::npos ||
            nm.find("crackle") != std::string::npos) {
          burble_subs.push_back(si);
        } else if (nm.find("backfire") != std::string::npos || nm.find("pop") != std::string::npos ||
                   nm.find("bang") != std::string::npos || nm.find("shift") != std::string::npos) {
          backfire_subs.push_back(si);
        }
      }
      if (burble_subs.empty() && backfire_subs.empty()) {
        if (total >= 2) {
          const uint32_t split = total / 2;
          for (uint32_t si = 0; si < split; ++si) burble_subs.push_back(si);
          for (uint32_t si = split; si < total; ++si) backfire_subs.push_back(si);
        } else {
          burble_subs.push_back(0);
          backfire_subs.push_back(0);
        }
      } else {
        if (burble_subs.empty()) burble_subs = backfire_subs;
        if (backfire_subs.empty()) backfire_subs = burble_subs;
      }
      static constexpr SfxSoundId kBurbleIds[4] = {SfxSoundId::kBurblePop0, SfxSoundId::kBurblePop1,
                                                   SfxSoundId::kBurblePop2, SfxSoundId::kBurblePop3};
      static constexpr SfxSoundId kBackfireIds[4] = {SfxSoundId::kBackfirePop, SfxSoundId::kBackfirePop1,
                                                     SfxSoundId::kBackfirePop2, SfxSoundId::kBackfirePop3};
      for (uint32_t i = 0; i < std::min<uint32_t>(4u, uint32_t(burble_subs.size())); ++i) {
        jobs.push_back({kBurbleIds[i], bpath, burble_subs[i]});
      }
      for (uint32_t i = 0; i < std::min<uint32_t>(4u, uint32_t(backfire_subs.size())); ++i) {
        jobs.push_back({kBackfireIds[i], bpath, backfire_subs[i]});
      }
    }
  }

  // Deterministic multi-RPM 3D opponent archetype pool (I4, V6, V8, V10/V12, Turbo) and traffic loops.
  // Reads each FSB4 header so subsound indices are always clamped safely across banks with 3..17 samples.
  {
    const auto lod1 = audio_dir / "cars" / "Engines" / "Soundbanks" / "LOD1";
    std::error_code ec;
    std::vector<std::filesystem::path> all_fsbs;
    if (std::filesystem::is_directory(lod1, ec)) {
      for (const auto& e : std::filesystem::directory_iterator(lod1, ec)) {
        if (!e.is_regular_file(ec) || lower_str(e.path().extension().string()) != ".fsb") continue;
        all_fsbs.push_back(e.path());
      }
      std::sort(all_fsbs.begin(), all_fsbs.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return lower_str(a.filename().string()) < lower_str(b.filename().string());
      });
    }
    std::array<std::filesystem::path, 5> archetype_banks{};
    std::vector<std::filesystem::path> fallback_exh;
    std::filesystem::path traffic_bank;
    for (const auto& p : all_fsbs) {
      const std::string fn = lower_str(p.filename().string());
      if (fn.find("traffic") != std::string::npos) {
        if (traffic_bank.empty()) traffic_bank = p;
        continue;
      }
      if (fn.find("_exh.fsb") == std::string::npos) continue;
      fallback_exh.push_back(p);
      auto assign_if_empty = [&](size_t slot) {
        if (archetype_banks[slot].empty()) archetype_banks[slot] = p;
      };
      if (fn.find("10c") != std::string::npos || fn.find("12c") != std::string::npos ||
          fn.find("v10") != std::string::npos || fn.find("v12") != std::string::npos ||
          fn.find("10n") != std::string::npos || fn.find("12n") != std::string::npos) {
        assign_if_empty(3);
      } else if (fn.find("8c") != std::string::npos || fn.find("8n") != std::string::npos ||
                 fn.find("8s") != std::string::npos || fn.find("v8") != std::string::npos) {
        assign_if_empty(2);
      } else if (fn.find("6t") != std::string::npos || fn.find("4t") != std::string::npos ||
                 fn.find("turbo") != std::string::npos) {
        assign_if_empty(4);
      } else if (fn.find("6c") != std::string::npos || fn.find("6n") != std::string::npos ||
                 fn.find("6f") != std::string::npos || fn.find("v6") != std::string::npos) {
        assign_if_empty(1);
      } else if (fn.find("4c") != std::string::npos || fn.find("4n") != std::string::npos ||
                 fn.find("i4") != std::string::npos) {
        assign_if_empty(0);
      }
    }
    // Fill any unpopulated archetype slot deterministically from remaining sorted _exh.fsb banks.
    size_t fb_cursor = 0;
    for (size_t slot = 0; slot < archetype_banks.size() && !fallback_exh.empty(); ++slot) {
      if (!archetype_banks[slot].empty()) continue;
      for (size_t tries = 0; tries < fallback_exh.size(); ++tries) {
        const auto& cand = fallback_exh[(fb_cursor + tries) % fallback_exh.size()];
        bool used = false;
        for (const auto& ab : archetype_banks)
          if (ab == cand) used = true;
        if (!used) {
          archetype_banks[slot] = cand;
          fb_cursor = (fb_cursor + tries + 1) % fallback_exh.size();
          break;
        }
      }
      if (archetype_banks[slot].empty()) archetype_banks[slot] = fallback_exh[slot % fallback_exh.size()];
    }

    auto queue_two_band_bank = [&](const std::filesystem::path& bank, SfxSoundId lo_id, SfxSoundId hi_id) {
      if (bank.empty()) return;
      std::vector<uint8_t> raw;
      std::vector<FsbSample> fsb_s;
      uint32_t n = 0;
      if (read_bytes(bank, raw) && read_fsb4(raw, fsb_s, nullptr)) n = uint32_t(fsb_s.size());
      if (n == 0) {
        jobs.push_back({lo_id, bank, 0u});
        return;
      }
      const uint32_t lo_sub =
          (n <= 1u) ? 0u : std::min<uint32_t>(n - 1u, std::max<uint32_t>(1u, n / 3u));
      const uint32_t hi_sub =
          (n <= 2u) ? (n - 1u) : std::min<uint32_t>(n - 1u, (n * 3u) / 4u);
      jobs.push_back({lo_id, bank, lo_sub});
      if (hi_sub != lo_sub) jobs.push_back({hi_id, bank, hi_sub});
    };

    static constexpr SfxSoundId kOppLoIds[5] = {SfxSoundId::kOtherCarLoop, SfxSoundId::kOtherCarLoop1,
                                                SfxSoundId::kOtherCarLoop2, SfxSoundId::kOtherCarLoop3,
                                                SfxSoundId::kOtherCarLoop4};
    static constexpr SfxSoundId kOppHiIds[5] = {SfxSoundId::kOtherCarLoopHi, SfxSoundId::kOtherCarLoop1Hi,
                                                SfxSoundId::kOtherCarLoop2Hi, SfxSoundId::kOtherCarLoop3Hi,
                                                SfxSoundId::kOtherCarLoop4Hi};
    for (size_t oi = 0; oi < archetype_banks.size(); ++oi) {
      queue_two_band_bank(archetype_banks[oi], kOppLoIds[oi], kOppHiIds[oi]);
    }
    if (!traffic_bank.empty()) {
      queue_two_band_bank(traffic_bank, SfxSoundId::kTrafficCarLoop, SfxSoundId::kTrafficCarLoopHi);
    } else if (!archetype_banks[0].empty()) {
      queue_two_band_bank(archetype_banks[0], SfxSoundId::kTrafficCarLoop, SfxSoundId::kTrafficCarLoopHi);
    }
  }

  // Airborne Showcase aircraft audio from Triggerable_Events.fev / Triggerable_Events.fsb
  // (P-51 Mustang, Biplane, Helicopter, Hot-Air Balloon; Jet only if a genuine jet asset exists,
  // never aliased onto P-51 propeller audio).
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
        } else if (!have_jet && (lp.find("jet") != std::string::npos || lp.find("turbine") != std::string::npos) &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseJet, bank, sub});
          have_jet = true;
        } else if (!have_bi && lp.find("biplane") != std::string::npos &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseBiplane, bank, sub});
          have_bi = true;
        } else if (!have_heli && lp.find("heli") != std::string::npos &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseHeli, bank, sub});
          have_heli = true;
        } else if (!have_bal && (lp.find("balloon") != std::string::npos || lp.find("baloon") != std::string::npos) &&
                   resolve_event_sample(trig_fev, ev.path, bank, sub)) {
          jobs.push_back({SfxSoundId::kShowcaseBalloon, bank, sub});
          have_bal = true;
        }
      }
    }
    const auto trig_fsb = cars::mods::resolve(trig_dir / "Triggerable_Events.fsb");
    std::error_code ec;
    if (std::filesystem::exists(trig_fsb, ec) && (!have_p51 || !have_bi || !have_heli || !have_bal || !have_jet)) {
      std::vector<uint8_t> raw;
      std::vector<FsbSample> fsb_s;
      if (read_bytes(trig_fsb, raw) && read_fsb4(raw, fsb_s, nullptr)) {
        for (uint32_t i = 0; i < fsb_s.size(); ++i) {
          const std::string nm = lower_str(fsb_s[i].name);
          if (!have_p51 && nm.find("p51") != std::string::npos) {
            jobs.push_back({SfxSoundId::kShowcaseP51, trig_fsb, i});
            have_p51 = true;
          } else if (!have_jet && (nm.find("jet") != std::string::npos || nm.find("turbine") != std::string::npos)) {
            jobs.push_back({SfxSoundId::kShowcaseJet, trig_fsb, i});
            have_jet = true;
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

  const size_t sound_jobs = jobs.size();
  jobs.insert(jobs.end(), ui_jobs.begin(), ui_jobs.end());
  if (note)
    *note = "sfx engine: queued " + std::to_string(sound_jobs) + " FEV/FSB4 sounds and " +
            std::to_string(ui_jobs.size()) + " ui4audio samples for async decode";
  if (sound_jobs == 0) populate_procedural_fallbacks();
  if (!jobs.empty()) start_decoder_worker(std::move(jobs), cache_dir, game_root);
  if (!state_root.empty()) {
    load_rally_sfx(state_root, cache_dir, nullptr);
  }
  return true;
}

bool SfxEngine::load_rally_sfx(const std::filesystem::path& rally_root_or_state_root,
                               const std::filesystem::path& cache_dir, std::string* note) {
  reclaim_retired();
  if (rally_root_or_state_root.empty()) return false;
  std::error_code ec;

  // Locate Rally audio root directories containing Rally_Tires.fev/.fsb and/or Car_Landings.fev/.fsb.
  std::vector<std::filesystem::path> audio_roots;
  auto push_if_dir = [&](const std::filesystem::path& p) {
    if (!p.empty() && std::filesystem::is_directory(p, ec)) audio_roots.push_back(p);
  };
  push_if_dir(rally_root_or_state_root);
  push_if_dir(rally_root_or_state_root / "audio");
  push_if_dir(rally_root_or_state_root / "Media" / "audio");
  push_if_dir(rally_root_or_state_root / "media" / "audio");
  push_if_dir(rally_root_or_state_root / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "audio");
  auto scan_dlczips = [&](const std::filesystem::path& dz) {
    if (!std::filesystem::is_directory(dz, ec)) return;
    for (const auto& z : std::filesystem::directory_iterator(dz, ec)) {
      if (!z.is_directory(ec)) continue;
      push_if_dir(z.path() / "Media" / "audio");
      push_if_dir(z.path() / "media" / "audio");
    }
  };
  scan_dlczips(rally_root_or_state_root / "Media" / "DLCZips");
  for (const auto& pr : {rally_root_or_state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002",
                         rally_root_or_state_root / "00000002", rally_root_or_state_root}) {
    if (!std::filesystem::is_directory(pr, ec)) continue;
    for (const auto& pkg : std::filesystem::directory_iterator(pr, ec)) {
      if (pkg.is_directory(ec)) scan_dlczips(pkg.path() / "Media" / "DLCZips");
    }
  }

  std::vector<DecodeJob> jobs;
  auto has_job = [&](SfxSoundId id) {
    for (const auto& j : jobs)
      if (j.id == id) return true;
    return false;
  };
  auto push_job = [&](SfxSoundId id, const std::filesystem::path& bank, uint32_t sub) {
    if (!has_job(id) && !bank.empty() && std::filesystem::exists(bank, ec)) {
      jobs.push_back({id, bank, sub});
    }
  };

  auto scan_bank_by_name = [&](const std::filesystem::path& fev_path, const std::filesystem::path& fsb_path,
                               auto&& classify_fn) {
    FevProject proj;
    if (!fev_path.empty() && std::filesystem::exists(fev_path, ec) && load_fev(fev_path, proj, nullptr)) {
      for (const auto& ev : proj.events) {
        std::filesystem::path bank;
        uint32_t sub = 0;
        if (!resolve_event_sample(proj, ev.path, bank, sub)) continue;
        classify_fn(lower_str(ev.path), bank, sub);
      }
    }
    if (!fsb_path.empty() && std::filesystem::exists(fsb_path, ec)) {
      std::vector<uint8_t> raw;
      std::vector<FsbSample> fsb_s;
      if (read_bytes(fsb_path, raw) && read_fsb4(raw, fsb_s, nullptr)) {
        for (uint32_t i = 0; i < fsb_s.size(); ++i) {
          classify_fn(lower_str(fsb_s[i].name), fsb_path, i);
        }
      }
    }
  };

  // 1. Rally_Tires.fev/.fsb
  for (const auto& ar : audio_roots) {
    const std::filesystem::path tire_dirs[] = {
        ar / "cars" / "Tires",
        ar / "cars" / "Rally_Tires",
        ar / "Tires",
        ar,
    };
    for (const auto& d : tire_dirs) {
      const auto fev_p = d / "Rally_Tires.fev";
      const auto fsb_p = d / "Rally_Tires.fsb";
      if (!std::filesystem::exists(fev_p, ec) && !std::filesystem::exists(fsb_p, ec)) continue;
      scan_bank_by_name(fev_p, fsb_p, [&](const std::string& nm, const std::filesystem::path& b, uint32_t sub) {
        const bool is_skid = (nm.find("skid") != std::string::npos || nm.find("slip") != std::string::npos ||
                              nm.find("slide") != std::string::npos);
        if (nm.find("mud") != std::string::npos || nm.find("wet") != std::string::npos) {
          push_job(is_skid ? SfxSoundId::kRallyTireSkidMud : SfxSoundId::kRallyTireRollMud, b, sub);
        } else if (nm.find("gravel") != std::string::npos || nm.find("loose") != std::string::npos) {
          push_job(is_skid ? SfxSoundId::kRallyTireSkidGravel : SfxSoundId::kRallyTireRollGravel, b, sub);
        } else if (nm.find("dirt") != std::string::npos || nm.find("offroad") != std::string::npos ||
                   nm.find("sand") != std::string::npos) {
          push_job(is_skid ? SfxSoundId::kRallyTireSkidDirt : SfxSoundId::kRallyTireRollDirt, b, sub);
        } else if (nm.find("tarmac") != std::string::npos || nm.find("asphalt") != std::string::npos ||
                   nm.find("onroad") != std::string::npos) {
          push_job(is_skid ? SfxSoundId::kRallyTireSkidTarmac : SfxSoundId::kRallyTireRollTarmac, b, sub);
        } else {
          push_job(is_skid ? SfxSoundId::kRallyTireSkidGravel : SfxSoundId::kRallyTireRollGravel, b, sub);
        }
      });
    }
  }

  // 2. Car_Landings.fev/.fsb
  for (const auto& ar : audio_roots) {
    const std::filesystem::path land_dirs[] = {
        ar / "cars" / "Car_Landings",
        ar / "cars" / "Suspension",
        ar / "cars",
        ar / "collisions",
        ar,
    };
    for (const auto& d : land_dirs) {
      const auto fev_p = d / "Car_Landings.fev";
      const auto fsb_p = d / "Car_Landings.fsb";
      if (!std::filesystem::exists(fev_p, ec) && !std::filesystem::exists(fsb_p, ec)) continue;
      scan_bank_by_name(fev_p, fsb_p, [&](const std::string& nm, const std::filesystem::path& b, uint32_t sub) {
        if (nm.find("heavy") != std::string::npos || nm.find("hard") != std::string::npos ||
            nm.find("large") != std::string::npos || nm.find("big") != std::string::npos) {
          push_job(SfxSoundId::kCarLandingHeavy, b, sub);
        } else if (nm.find("dirt") != std::string::npos || nm.find("gravel") != std::string::npos ||
                   nm.find("offroad") != std::string::npos || nm.find("mud") != std::string::npos) {
          push_job(SfxSoundId::kCarLandingDirt, b, sub);
        } else {
          push_job(SfxSoundId::kCarLandingLight, b, sub);
        }
      });
      // Fallback: if Car_Landings.fsb exists but names didn't distinguish light/heavy, bind subsound 0.
      if (std::filesystem::exists(fsb_p, ec)) {
        push_job(SfxSoundId::kCarLandingLight, fsb_p, 0);
      }
    }
  }

  if (jobs.empty()) {
    if (note) *note = "rally sfx: no Rally_Tires or Car_Landings banks found";
    return false;
  }

  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  stop_rally_worker_.store(true, std::memory_order_release);
  if (rally_worker_.joinable()) rally_worker_.join();
  stop_rally_worker_.store(false, std::memory_order_release);
  const size_t n_jobs = jobs.size();
  decoders_running_.fetch_add(1, std::memory_order_acq_rel);
  rally_worker_ = std::thread([this, jobs = std::move(jobs), cache_dir]() {
    const DecoderRunning running{decoders_running_};
    lower_decoder_thread_priority();
    const std::filesystem::path ffmpeg = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
    std::error_code tec;
    const std::filesystem::path sfx_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "sfx_rally");
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(tec) /
        ("shift-rally-sfx-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, tec);
    for (const auto& job : jobs) {
      if (stop_rally_worker_.load(std::memory_order_acquire)) break;
      const std::string stem_low = lower_str(job.bank_path.stem().string());
      const std::filesystem::path cache_file =
          sfx_cache.empty() ? std::filesystem::path{}
                            : (sfx_cache / (stem_low + "." + std::to_string(job.subsound) + ".s16"));
      auto pcm = std::make_unique<PcmSample>();
      bool ok = !cache_file.empty() && read_cached_sfx(cache_file, *pcm);
      if (!ok) {
        ok = decode_fsb_subsound(job.bank_path, job.subsound, ffmpeg, temp_dir, *pcm);
        if (ok && !cache_file.empty()) write_cached_sfx(cache_file, *pcm);
      }
      if (!ok) ok = synthesize_fallback_sfx(job.id, 24000, *pcm);
      if (ok && !pcm->data.empty()) {
        PcmSample* raw_pcm = pcm.release();
        if (!rally_loaded_q_.push({job.id, raw_pcm})) delete raw_pcm;
      }
    }
    std::filesystem::remove_all(temp_dir, tec);
  });
  if (note) *note = "rally sfx: queued " + std::to_string(n_jobs) + " Rally_Tires/Car_Landings sounds";
  return true;
}

void SfxEngine::reload_burble_bank(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                                   const std::string& burble_bank, int burble_count, int backfire_count) {
  reclaim_retired();
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
  const uint32_t total =
      (read_bytes(bpath, raw) && read_fsb4(raw, fsb_s, nullptr)) ? uint32_t(fsb_s.size()) : 1u;

  std::vector<uint32_t> burble_subs;
  std::vector<uint32_t> backfire_subs;
  for (uint32_t si = 0; si < fsb_s.size(); ++si) {
    const std::string nm = lower_str(fsb_s[si].name);
    if (nm.find("burble") != std::string::npos || nm.find("overrun") != std::string::npos ||
        nm.find("crackle") != std::string::npos) {
      burble_subs.push_back(si);
    } else if (nm.find("backfire") != std::string::npos || nm.find("pop") != std::string::npos ||
               nm.find("bang") != std::string::npos || nm.find("shift") != std::string::npos) {
      backfire_subs.push_back(si);
    }
  }
  if (burble_subs.empty() && backfire_subs.empty()) {
    if (burble_count > 0 && backfire_count > 0 && total > 1) {
      const uint32_t nb = std::min<uint32_t>(uint32_t(burble_count), total - 1);
      const uint32_t nf = std::min<uint32_t>(uint32_t(backfire_count), total - nb);
      for (uint32_t si = 0; si < nb; ++si) burble_subs.push_back(si);
      for (uint32_t si = nb; si < nb + nf; ++si) backfire_subs.push_back(si);
    } else if (total >= 2) {
      const uint32_t split = total / 2;
      for (uint32_t si = 0; si < split; ++si) burble_subs.push_back(si);
      for (uint32_t si = split; si < total; ++si) backfire_subs.push_back(si);
    } else {
      burble_subs.push_back(0);
      backfire_subs.push_back(0);
    }
  } else {
    if (burble_subs.empty()) burble_subs = backfire_subs;
    if (backfire_subs.empty()) backfire_subs = burble_subs;
  }

  static constexpr SfxSoundId kBurbleIds[4] = {SfxSoundId::kBurblePop0, SfxSoundId::kBurblePop1,
                                               SfxSoundId::kBurblePop2, SfxSoundId::kBurblePop3};
  static constexpr SfxSoundId kBackfireIds[4] = {SfxSoundId::kBackfirePop, SfxSoundId::kBackfirePop1,
                                                 SfxSoundId::kBackfirePop2, SfxSoundId::kBackfirePop3};
  for (uint32_t i = 0; i < std::min<uint32_t>(4u, uint32_t(burble_subs.size())); ++i) {
    jobs.push_back({kBurbleIds[i], bpath, burble_subs[i]});
  }
  for (uint32_t i = 0; i < std::min<uint32_t>(4u, uint32_t(backfire_subs.size())); ++i) {
    jobs.push_back({kBackfireIds[i], bpath, backfire_subs[i]});
  }
  if (jobs.empty()) return;

  stop_burble_worker_.store(false, std::memory_order_release);
  decoders_running_.fetch_add(1, std::memory_order_acq_rel);
  burble_worker_ = std::thread([this, jobs = std::move(jobs), cache_dir, game_root]() {
    const DecoderRunning running{decoders_running_};
    lower_decoder_thread_priority();
    const std::filesystem::path ffmpeg = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
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
  decoders_running_.fetch_add(1, std::memory_order_acq_rel);
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir = std::move(cache_dir),
                         game_root = std::move(game_root)]() {
    const DecoderRunning running{decoders_running_};
    lower_decoder_thread_priority();
    const std::filesystem::path ffmpeg = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
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
      // A UI catalog event plays its own sample or nothing.
      if (!ok && job.ui < 0 && job.ev < 0) ok = synthesize_fallback_sfx(job.id, 24000, *pcm);
      if (ok && !pcm->data.empty()) {
        PcmSample* raw = pcm.release();
        if (!loaded_q_.push({job.id, raw, job.ui, job.ev})) delete raw;
      }
    }
    std::filesystem::remove_all(temp_dir, ec);
  });
}

void SfxEngine::spawn_one_shot(SfxSoundId id, Bus bus, float gain, float left, float right, float pitch, int ui) {
  if (gain <= 1e-4f) return;
  if (ui >= 0) {
    if (ui >= kMaxUiSamples || !ui_samples_[size_t(ui)] || ui_samples_[size_t(ui)]->data.empty()) return;
  } else {
    const size_t idx = size_t(id);
    if (idx >= samples_.size() || !samples_[idx] || samples_[idx]->data.empty()) return;
  }
  // UI events are sparse and must not be stolen by tyre or impact ticks.
  const uint8_t prio = ui >= 0 ? uint8_t(100) : one_shot_priority(id);
  const float clamped_pitch = std::clamp(pitch, 0.25f, 4.0f);
  OneShot* slot = nullptr;
  for (auto& o : one_shots_) {
    if (!o.active) {
      slot = &o;
      break;
    }
  }
  if (slot) {
    slot->active = true;
    slot->id = id;
    slot->ui = ui;
    slot->bus = bus;
    slot->phase = 0.0;
    slot->gain = gain;
    slot->left = left;
    slot->right = right;
    slot->pitch = clamped_pitch;
    slot->priority = prio;
    slot->spawn_seq = ++one_shot_seq_;
    slot->stealing = false;
    slot->fade_rem_frames = 0;
    slot->fade_total_frames = 0;
    last_one_shot_id_.store(id, std::memory_order_release);
    last_one_shot_gain_.store(gain, std::memory_order_release);
    last_one_shot_pitch_.store(clamped_pitch, std::memory_order_release);
    return;
  }

  // All voice slots occupied: steal the lowest-priority, oldest eligible voice with a 5 ms fade-out.
  // Never steal a higher-priority voice (e.g. tire ticks cannot kill Countdown321/RaceFinish/ImpactLfe).
  OneShot* victim = nullptr;
  uint8_t best_prio = 255;
  uint64_t best_seq = UINT64_MAX;
  for (auto& o : one_shots_) {
    const uint8_t eff_prio = o.stealing ? o.next_priority : o.priority;
    const uint64_t eff_seq = o.stealing ? o.next_spawn_seq : o.spawn_seq;
    if (eff_prio > prio) continue;
    if (!victim || eff_prio < best_prio || (eff_prio == best_prio && eff_seq < best_seq)) {
      victim = &o;
      best_prio = eff_prio;
      best_seq = eff_seq;
    }
  }
  if (!victim) return;  // drop incoming low-priority sound cleanly

  const int fade_frames = std::max(1, int(std::lround(0.005 * double(rate_))));
  if (!victim->stealing) {
    victim->stealing = true;
    victim->fade_total_frames = fade_frames;
    victim->fade_rem_frames = fade_frames;
  }
  victim->next_id = id;
  victim->next_ui = ui;
  victim->next_bus = bus;
  victim->next_gain = gain;
  victim->next_left = left;
  victim->next_right = right;
  victim->next_pitch = clamped_pitch;
  victim->next_priority = prio;
  victim->next_spawn_seq = ++one_shot_seq_;
  last_one_shot_id_.store(id, std::memory_order_release);
  last_one_shot_gain_.store(gain, std::memory_order_release);
  last_one_shot_pitch_.store(clamped_pitch, std::memory_order_release);
}

int16_t SfxEngine::map_surface_index(std::string_view physics_surface) const {
  if (const auto* entry = tires_cfg_.find_surface(physics_surface)) {
    return int16_t(entry->surface_index);
  }
  const std::string raw_low = lower_str(std::string(physics_surface));
  if (raw_low.find("leaf") != std::string::npos || raw_low.find("leaves") != std::string::npos ||
      raw_low.find("foliage") != std::string::npos) {
    return 51;
  }
  if (raw_low.find("mud") != std::string::npos) return 20;
  if (raw_low.find("grass") != std::string::npos) return 15;
  if (raw_low.find("dirt") != std::string::npos || raw_low.find("gravel") != std::string::npos ||
      raw_low.find("sand") != std::string::npos) {
    return 10;
  }
  if (raw_low.find("brick") != std::string::npos || raw_low.find("cobble") != std::string::npos) return 30;
  if (raw_low.find("wood") != std::string::npos || raw_low.find("track") != std::string::npos ||
      raw_low.find("metal") != std::string::npos) {
    return 40;
  }
  return 0;
}

uint8_t SfxEngine::map_surface_group(std::string_view physics_surface) const {
  const std::string raw_low = lower_str(std::string(physics_surface));
  if (raw_low.find("mud") != std::string::npos) return 5;
  if (const auto* entry = tires_cfg_.find_surface(physics_surface)) {
    const std::string g = lower_str(entry->lod1_group);
    if (g.find("mud") != std::string::npos) return 5;
    if (g.find("offroad") != std::string::npos || g.find("dirt") != std::string::npos ||
        g.find("gravel") != std::string::npos || g.find("sand") != std::string::npos) {
      return 1;
    }
    if (g.find("grass") != std::string::npos) return 2;
    if (g.find("brick") != std::string::npos || g.find("cobble") != std::string::npos) return 3;
    if (g.find("trackway") != std::string::npos || g.find("wood") != std::string::npos) return 4;
    return 0;
  }
  if (raw_low.find("grass") != std::string::npos) return 2;
  if (raw_low.find("dirt") != std::string::npos || raw_low.find("gravel") != std::string::npos ||
      raw_low.find("sand") != std::string::npos) {
    return 1;
  }
  if (raw_low.find("brick") != std::string::npos || raw_low.find("cobble") != std::string::npos) return 3;
  if (raw_low.find("wood") != std::string::npos || raw_low.find("track") != std::string::npos ||
      raw_low.find("metal") != std::string::npos) {
    return 4;
  }
  return 0;
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
  const float pitch_var = 0.92f + 0.16f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
  if (hit.is_break) {
    const SfxSoundId mat_id = smash_material_sound_id(hit.smash_material, hit.surface_hint);
    SfxSoundId break_id = resolve_fallback(mat_id, SfxSoundId::kBreakProp);
    if (mat_id == SfxSoundId::kBreakGlass && break_id == SfxSoundId::kBreakProp) {
      break_id = resolve_fallback(SfxSoundId::kGlassShatter, SfxSoundId::kBreakProp);
    }
    spawn_one_shot(break_id, Bus::kCollision, 0.75f * intensity * atten, pl, pr, pitch_var);
  } else {
    // Resolve surface-pair impact via CollisionData.xml where available, with surface_hint fallback.
    std::string_view surf_name = "Asphalt";
    if (hit.is_car_on_car || hit.surface_hint == 1) surf_name = "CarAluminum";
    else if (hit.surface_hint == 2) surf_name = "Dirt";
    else if (hit.surface_hint == 3) surf_name = "Metal";
    else if (hit.surface_hint == 4) surf_name = "Wood";

    const CollisionEventSpec* pair = coll_cfg_.find_pair(surf_name, "CarAluminum");
    SfxSoundId imp_id = SfxSoundId::kImpact3d;
    bool emit_lfe = true;
    if (pair && !pair->impact3d.empty()) {
      const std::string ev_low = lower_str(pair->impact3d);
      if (ev_low.find("caraluminum") != std::string::npos && (hit.is_car_on_car || hit.surface_hint == 1)) {
        imp_id = resolve_fallback(SfxSoundId::kImpactCarOnCar, SfxSoundId::kImpact3d);
      } else if (ev_low.find("dirt") != std::string::npos || ev_low.find("grass") != std::string::npos ||
                 ev_low.find("gravel") != std::string::npos || ev_low.find("sand") != std::string::npos) {
        imp_id = resolve_fallback(SfxSoundId::kImpactDirt, SfxSoundId::kImpact3d);
      } else if (ev_low.find("metal") != std::string::npos || ev_low.find("guard") != std::string::npos ||
                 ev_low.find("steel") != std::string::npos) {
        imp_id = resolve_fallback(SfxSoundId::kImpactMetal, SfxSoundId::kImpact3d);
      } else if (ev_low.find("wood") != std::string::npos || ev_low.find("tree") != std::string::npos) {
        imp_id = resolve_fallback(SfxSoundId::kImpactWood, SfxSoundId::kImpact3d);
      } else {
        imp_id = SfxSoundId::kImpact3d;
      }
      if (pair->impact_lfe.empty()) {
        emit_lfe = false;
      } else {
        const std::string lfe_low = lower_str(pair->impact_lfe);
        if (lfe_low == "none" || lfe_low == "silent") emit_lfe = false;
      }
    } else {
      if (hit.is_car_on_car || hit.surface_hint == 1) {
        imp_id = resolve_fallback(SfxSoundId::kImpactCarOnCar, SfxSoundId::kImpact3d);
      } else if (hit.surface_hint == 2) {
        imp_id = resolve_fallback(SfxSoundId::kImpactDirt, SfxSoundId::kImpact3d);
      } else if (hit.surface_hint == 3) {
        imp_id = resolve_fallback(SfxSoundId::kImpactMetal, SfxSoundId::kImpact3d);
      } else if (hit.surface_hint == 4) {
        imp_id = resolve_fallback(SfxSoundId::kImpactWood, SfxSoundId::kImpact3d);
      }
    }
    spawn_one_shot(imp_id, Bus::kCollision, 0.85f * intensity * atten, pl, pr, pitch_var);
    if (emit_lfe) {
      spawn_one_shot(SfxSoundId::kImpactLfe, Bus::kCollision, 0.65f * intensity, 0.707f, 0.707f);
    }
  }
}

void SfxEngine::trigger_landing(const LandingHit& hit, const VehicleAudioState& st) {
  drain_loaded();
  if (time_s_ - last_landing_s_ < 0.12) return;
  const float speed = std::max(0.0f, std::isfinite(hit.impact_speed_mps) ? hit.impact_speed_mps : 0.0f);
  const float air = std::max(0.0f, std::isfinite(hit.air_time_s) ? hit.air_time_s : 0.0f);
  if (speed < 1.2f && air < 0.15f) return;
  last_landing_s_ = time_s_;
  const float intensity = std::clamp(std::max(speed / 8.0f, air / 1.0f), 0.22f, 1.0f);
  float pl = 0.707f, pr = 0.707f;
  compute_stereo_pan(st.listener_pos, st.listener_right, hit.pos, pl, pr);
  const float susp_scale = safe_clamp_f(st.susp_vol_scale, 0.0f, 2.5f, 1.0f);
  const float pitch_var = 0.94f + 0.12f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
  const bool heavy = (speed >= 5.5f || air >= 0.70f);
  SfxSoundId preferred = SfxSoundId::kCarLandingLight;
  if (heavy) preferred = SfxSoundId::kCarLandingHeavy;
  else if (hit.surface_hint >= 1) preferred = SfxSoundId::kCarLandingDirt;
  const SfxSoundId land_id =
      resolve_fallback(preferred, resolve_fallback(SfxSoundId::kCarLandingLight, SfxSoundId::kSuspensionBottom));
  spawn_one_shot(land_id, Bus::kSuspension, 0.80f * intensity * susp_scale, pl, pr, pitch_var);
  if (heavy) {
    spawn_one_shot(resolve_fallback(SfxSoundId::kSuspensionBottom, SfxSoundId::kImpactLfe), Bus::kSuspension,
                   0.60f * intensity * susp_scale, 0.707f, 0.707f, 0.92f);
  }
}

void SfxEngine::trigger_ui_sample(int sample, float gain, float left, float right) {
  drain_loaded();
  if (sample < 0 || sample >= kMaxUiSamples || !std::isfinite(gain)) return;
  spawn_one_shot(SfxSoundId::kUiAccept, Bus::kUi, std::clamp(gain, 0.0f, 4.0f), safe_clamp_f(left, 0.0f, 2.0f, 0.707f),
                 safe_clamp_f(right, 0.0f, 2.0f, 0.707f), 1.0f, sample);
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
    case UiCue::kPause: id = resolve_fallback(SfxSoundId::kUiPause, SfxSoundId::kUiMenuOpen); break;
    case UiCue::kUnPause: id = resolve_fallback(SfxSoundId::kUiUnPause, SfxSoundId::kUiCancel); break;
    case UiCue::kVscroll:
      id = resolve_fallback(SfxSoundId::kUiVscroll, SfxSoundId::kUiMove);
      gain = 0.45f;
      break;
    case UiCue::kRewindTapeStart: id = SfxSoundId::kRewindTapeStart; gain = 0.85f; break;
    case UiCue::kRewindTapeStop:
      id = resolve_fallback(SfxSoundId::kRewindTapeStop, SfxSoundId::kRewindTapeStart);
      gain = 0.80f;
      break;
    case UiCue::kCountdown321: id = SfxSoundId::kCountdown321; gain = 0.85f; break;
    case UiCue::kCountdownTick: {
      // Suppress individual digit ticks while the full 3-2-1 pre-race track is already playing.
      for (const auto& o : one_shots_) {
        if (o.active && (o.id == SfxSoundId::kCountdown321 || (o.stealing && o.next_id == SfxSoundId::kCountdown321))) {
          return;
        }
      }
      id = resolve_fallback(SfxSoundId::kUiCountdownTick, SfxSoundId::kUiMove);
      gain = 0.78f;
      break;
    }
    case UiCue::kCountdownGo:
      id = resolve_fallback(SfxSoundId::kUiCountdownGo, SfxSoundId::kCountdown321);
      gain = 0.90f;
      break;
    case UiCue::kCheckpoint:
      id = resolve_fallback(SfxSoundId::kUiCheckpoint, SfxSoundId::kUiAccept);
      gain = 0.72f;
      break;
    case UiCue::kLap:
      id = resolve_fallback(SfxSoundId::kUiLap, SfxSoundId::kAchievementReward);
      gain = 0.85f;
      break;
    case UiCue::kWristbandUnlock:
      id = resolve_fallback(SfxSoundId::kUiWristbandUnlock, SfxSoundId::kAchievementReward);
      gain = 0.92f;
      break;
    case UiCue::kRaceFinish:
      id = resolve_fallback(SfxSoundId::kUiRaceFinish, SfxSoundId::kAchievementReward);
      gain = 0.90f;
      break;
    case UiCue::kSignSmash: id = SfxSoundId::kSignSmash; gain = 0.80f; break;
    case UiCue::kSkillAward:
      id = resolve_fallback(SfxSoundId::kUiSkillAward, SfxSoundId::kUiAccept);
      gain = 0.70f;
      break;
    case UiCue::kSkillMultiplier:
      id = resolve_fallback(SfxSoundId::kUiSkillMulti, SfxSoundId::kUiSkillAward);
      gain = 0.78f;
      break;
    case UiCue::kSkillBank:
      id = resolve_fallback(SfxSoundId::kUiSkillBank, SfxSoundId::kAchievementReward);
      gain = 0.80f;
      break;
    case UiCue::kSkillBreak:
      id = resolve_fallback(SfxSoundId::kUiSkillBreak, SfxSoundId::kUiError);
      gain = 0.75f;
      break;
    case UiCue::kRankUp:
      id = resolve_fallback(SfxSoundId::kUiRankUp, SfxSoundId::kAchievementReward);
      gain = 0.88f;
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
    case UiCue::kPhotoShutter:
      id = resolve_fallback(SfxSoundId::kUiPhotoShutter, SfxSoundId::kUiAccept);
      gain = 0.82f;
      break;
    case UiCue::kPurchase:
      id = resolve_fallback(SfxSoundId::kUiPurchase, SfxSoundId::kUiAccept);
      gain = 0.82f;
      break;
    case UiCue::kCarAdded:
      id = resolve_fallback(SfxSoundId::kUiCarAdded, SfxSoundId::kAchievementReward);
      gain = 0.85f;
      break;
    case UiCue::kH2hGo:
      id = resolve_fallback(SfxSoundId::kUiH2hGo, SfxSoundId::kUiCountdownGo);
      gain = 0.85f;
      break;
    case UiCue::kH2hLoss:
      id = resolve_fallback(SfxSoundId::kUiH2hLoss, SfxSoundId::kUiSkillBreak);
      gain = 0.80f;
      break;
  }
  spawn_one_shot(id, Bus::kUi, gain, 0.707f, 0.707f);
}

void SfxEngine::mix_loop(LoopVoice& v, SfxSoundId id, float target_gain, float target_pitch, float target_l,
                         float target_r, float* bus, int frames) {
  const size_t idx = size_t(id);
  mix_loop_pcm(v, idx < samples_.size() ? samples_[idx] : nullptr, target_gain, target_pitch, target_l, target_r, bus,
               frames);
}

namespace {
// The game scales its tyre events by tunables and per-car values it reads at
// run time (in its tyre audio level setup); this one level stands for their product (Q331).
constexpr float kTyreEventLevel = 0.83f;  // -1.6 dB: wheelspin on tarmac -0.8, on dirt -2.4
}  // namespace

float SfxEngine::mix_event(EventLoop& el, const std::array<float, int(EvParam::kCount)>& in, float gain, float pitch,
                           float left, float right, float* bus, int frames) {
  if (!el.ok) return 0.0f;
  std::array<float, 16> raw{};
  const size_t np = std::min(el.ev.params.size(), raw.size());
  for (size_t i = 0; i < np; ++i) raw[i] = el.ev.params[i].min_val;
  for (int p = 0; p < int(EvParam::kCount); ++p) {
    const int pi = el.param[size_t(p)];
    if (pi >= 0 && size_t(pi) < np) raw[size_t(pi)] = in[size_t(p)];
  }
  std::array<FevInstanceLevel, kMaxEventVoices> lv{};
  const int n = gain > 0.0f ? evaluate_fev_event(el.ev, raw.data(), np, lv.data(), kMaxEventVoices) : 0;
  const float ev_pitch = std::exp2(el.ev.pitch * kFevPitchOctaves) * pitch;
  std::array<float, kMaxEventVoices> tg{}, tp{};
  float total = 0.0f;
  bool audible = false;
  for (int i = 0; i < el.count; ++i) {
    const float g = i < n ? lv[size_t(i)].gain * el.tpl_gain[size_t(i)] * gain : 0.0f;
    float p = ev_pitch * el.tpl_pitch[size_t(i)];
    if (i < n && lv[size_t(i)].has_pitch_env) p *= std::exp2((lv[size_t(i)].pitch_env - 0.5f) * kFevPitchEnvOctaves);
    if (i >= n) p = el.voices[size_t(i)].pitch;
    tg[size_t(i)] = g;
    tp[size_t(i)] = p;
    total += g;
    audible = audible || g > 1e-4f || el.voices[size_t(i)].gain > 1e-4f;
  }
  auto mix_voices = [&](float* out, int count) {
    for (int i = 0; i < el.count; ++i) {
      const int s = el.slot[size_t(i)];
      mix_loop_pcm(el.voices[size_t(i)], s >= 0 ? event_samples_[size_t(s)] : nullptr, tg[size_t(i)], tp[size_t(i)],
                   left, right, out, count);
    }
  };
  if (el.hp_env < 0 || !bus) {
    mix_voices(bus, frames);
    return total;
  }
  if (!audible) {
    el.hp_z.fill(0.0f);
    return total;
  }
  // The event's "FMOD Highpass" cutoff: envelope y on a log axis from 10 Hz to
  // 22 kHz (Q331), a 2-pole RBJ high-pass (FMOD's default resonance, Q 1).
  const FevEnvelope& he = el.ev.layers.front().envelopes[size_t(el.hp_env)];
  const size_t hpi = he.param_index;
  const float hx = hpi < np ? fev_normalize(el.ev.params[hpi], raw[hpi]) : 0.0f;
  const float fc = std::clamp(10.0f * std::pow(2200.0f, std::clamp(he.eval(hx), 0.0f, 1.0f)), 10.0f,
                              0.45f * float(rate_));
  const float w0 = 2.0f * 3.14159265f * fc / float(rate_);
  const float alpha = std::sin(w0) / 2.0f;  // Q = 1
  const float cw = std::cos(w0);
  const float a0 = 1.0f + alpha;
  const float b0 = (1.0f + cw) / 2.0f / a0, b1 = -(1.0f + cw) / a0, b2 = b0;
  const float a1 = -2.0f * cw / a0, a2 = (1.0f - alpha) / a0;
  const int chunk_max = int(event_scratch_.size() / 2);
  for (int done = 0; done < frames;) {
    const int c = std::min(chunk_max, frames - done);
    float* tmp = event_scratch_.data();
    std::fill(tmp, tmp + 2 * c, 0.0f);
    mix_voices(tmp, c);
    for (int ch = 0; ch < 2; ++ch) {
      float* z = el.hp_z.data() + 4 * ch;  // x1 x2 y1 y2
      for (int k = 0; k < c; ++k) {
        const float x = tmp[2 * k + ch];
        const float y = b0 * x + b1 * z[0] + b2 * z[1] - a1 * z[2] - a2 * z[3];
        z[1] = z[0];
        z[0] = x;
        z[3] = z[2];
        z[2] = std::isfinite(y) ? y : 0.0f;
        bus[2 * (done + k) + ch] += z[2];
      }
    }
    done += c;
  }
  return total;
}

void SfxEngine::mix_loop_pcm(LoopVoice& v, const PcmSample* pcm, float target_gain, float target_pitch,
                             float target_l, float target_r, float* bus, int frames) {
  const float tg = safe_clamp_f(target_gain, 0.0f, 4.0f, 0.0f);
  const float tl = safe_clamp_f(target_l, 0.0f, 2.0f, 0.707f);
  const float tr = safe_clamp_f(target_r, 0.0f, 2.0f, 0.707f);
  if (!std::isfinite(v.gain) || v.gain < 0.0f) v.gain = 0.0f;
  if (!std::isfinite(v.pitch) || v.pitch < 0.25f) v.pitch = 1.0f;
  if (!std::isfinite(v.left)) v.left = tl;
  if (!std::isfinite(v.right)) v.right = tr;
  if (!std::isfinite(v.phase) || v.phase < 0.0) v.phase = 0.0;
  if (!bus || !pcm || pcm->data.empty() || (tg <= 1e-4f && v.gain <= 1e-4f)) {
    v.gain = 0.0f;
    return;
  }
  const size_t n = pcm->data.size();
  const double rate_ratio = double(std::max(1, pcm->rate)) / double(rate_);
  const float p_clamped = safe_clamp_f(target_pitch, 0.25f, 4.0f, 1.0f);
  for (int i = 0; i < frames; ++i) {
    v.gain += (tg - v.gain) * smooth_;
    v.pitch += (p_clamped - v.pitch) * smooth_;
    v.left += (tl - v.left) * smooth_;
    v.right += (tr - v.right) * smooth_;
    if (v.gain > 1e-5f) {
      const size_t k = size_t(v.phase);
      const double frac = v.phase - double(k);
      const float a = pcm->data[k % n];
      const float b = pcm->data[(k + 1) % n];
      const float s = (a + (b - a) * float(frac)) * v.gain;
      if (std::isfinite(s)) {
        bus[2 * i] += s * v.left;
        bus[2 * i + 1] += s * v.right;
      }
    }
    v.phase += double(v.pitch) * rate_ratio;
    if (v.phase >= double(n)) v.phase = std::fmod(v.phase, double(n));
  }
  if (v.gain < 1e-18f) v.gain = 0.0f;
}

void SfxEngine::mix_loop_mono(LoopVoice& v, SfxSoundId id, float target_gain, float target_pitch, float* mono_out,
                              int frames) {
  const size_t idx = size_t(id);
  const PcmSample* pcm = idx < samples_.size() ? samples_[idx] : nullptr;
  const float tg = safe_clamp_f(target_gain, 0.0f, 4.0f, 0.0f);
  if (!std::isfinite(v.gain) || v.gain < 0.0f) v.gain = 0.0f;
  if (!std::isfinite(v.pitch) || v.pitch < 0.25f) v.pitch = 1.0f;
  if (!std::isfinite(v.phase) || v.phase < 0.0) v.phase = 0.0;
  if (!mono_out || !pcm || pcm->data.empty() || (tg <= 1e-4f && v.gain <= 1e-4f)) {
    v.gain = 0.0f;
    return;
  }
  const size_t n = pcm->data.size();
  const double rate_ratio = double(std::max(1, pcm->rate)) / double(rate_);
  const float p_clamped = safe_clamp_f(target_pitch, 0.25f, 4.0f, 1.0f);
  for (int i = 0; i < frames; ++i) {
    v.gain += (tg - v.gain) * smooth_;
    v.pitch += (p_clamped - v.pitch) * smooth_;
    if (v.gain > 1e-5f) {
      const size_t k = size_t(v.phase);
      const double frac = v.phase - double(k);
      const float a = pcm->data[k % n];
      const float b = pcm->data[(k + 1) % n];
      const float s = (a + (b - a) * float(frac)) * v.gain;
      if (std::isfinite(s)) mono_out[i] += s;
    }
    v.phase += double(v.pitch) * rate_ratio;
    if (v.phase >= double(n)) v.phase = std::fmod(v.phase, double(n));
  }
  if (v.gain < 1e-18f) v.gain = 0.0f;
}

void SfxEngine::render(const VehicleAudioState& st, float* const* buses, int frames, EnginePlayer* focus_engine,
                       float* lfe_out) {
  if (frames <= 0) return;
  drain_loaded();
  drain_other_engines();
  const double prev_time_s = time_s_;
  const double dt = double(frames) / double(rate_);
  time_s_ += dt;

  // Per-camera / cockpit isolation scales from _CMT.xml <OtherVolumes> (each read independently).
  const float view_wind = safe_clamp_f(st.view_wind_scale, 0.0f, 2.5f, 1.0f);
  const float view_tyre = safe_clamp_f(st.view_tyre_scale, 0.0f, 2.5f, 1.0f);
  const float view_trans = safe_clamp_f(st.view_trans_scale, 0.0f, 2.5f, 1.0f);
  const float view_turbo = safe_clamp_f(st.view_turbo_scale, 0.0f, 2.5f, 1.0f);
  const float view_sc = safe_clamp_f(st.view_sc_scale, 0.0f, 2.5f, 1.0f);
  const float view_backfire = safe_clamp_f(st.view_backfire_scale, 0.0f, 2.5f, 1.0f);
  const float view_burble = safe_clamp_f(st.view_burble_scale, 0.0f, 2.5f, 1.0f);
  const float view_susp = safe_clamp_f(st.view_susp_scale, 0.0f, 2.5f, 1.0f);
  const float view_amb = safe_clamp_f(st.view_ambience_scale, 0.0f, 2.5f, 1.0f);
  const float view_other = safe_clamp_f(st.view_other_cars_scale, 0.0f, 2.5f, 1.0f);
  const float view_damage = safe_clamp_f(st.view_damage_scale, 0.0f, 2.5f, 1.0f);
  const float view_exh_noise = safe_clamp_f(st.view_exh_noise_scale, 0.0f, 2.5f, 1.0f);
  const float view_ev = safe_clamp_f(st.view_ev_scale, 0.0f, 2.5f, 1.0f);
  const float view_lfe = safe_clamp_f(st.view_lfe_scale, 0.0f, 2.5f, 1.0f);
  const float view_refl = safe_clamp_f(st.view_refl_scale, 0.0f, 2.5f, 1.0f);

  auto roll_for_surface = [&](uint8_t sgrp) -> SfxSoundId {
    if (st.rally_mode) {
      switch (sgrp) {
        case 1:
          return resolve_fallback(SfxSoundId::kRallyTireRollGravel,
                                  resolve_fallback(SfxSoundId::kRallyTireRollDirt,
                                                   resolve_fallback(SfxSoundId::kTireRollOffRoad,
                                                                    SfxSoundId::kTireRollOnRoad)));
        case 5:
          return resolve_fallback(SfxSoundId::kRallyTireRollMud,
                                  resolve_fallback(SfxSoundId::kRallyTireRollDirt,
                                                   resolve_fallback(SfxSoundId::kTireRollOffRoad,
                                                                    SfxSoundId::kTireRollOnRoad)));
        case 0:
          return resolve_fallback(SfxSoundId::kRallyTireRollTarmac, SfxSoundId::kTireRollOnRoad);
        default: break;
      }
    }
    switch (sgrp) {
      case 1: return resolve_fallback(SfxSoundId::kTireRollOffRoad, SfxSoundId::kTireRollOnRoad);
      case 2: return resolve_fallback(SfxSoundId::kTireRollGrass, SfxSoundId::kTireRollOnRoad);
      case 3: return resolve_fallback(SfxSoundId::kTireRollBrick, SfxSoundId::kTireRollOnRoad);
      case 4: return resolve_fallback(SfxSoundId::kTireRollTrackway, SfxSoundId::kTireRollOnRoad);
      case 5:
        return resolve_fallback(SfxSoundId::kRallyTireRollMud,
                                resolve_fallback(SfxSoundId::kTireRollOffRoad, SfxSoundId::kTireRollOnRoad));
      default: return SfxSoundId::kTireRollOnRoad;
    }
  };
  auto skid_for_surface = [&](uint8_t sgrp) -> SfxSoundId {
    if (st.rally_mode) {
      switch (sgrp) {
        case 1:
          return resolve_fallback(SfxSoundId::kRallyTireSkidGravel,
                                  resolve_fallback(SfxSoundId::kRallyTireSkidDirt,
                                                   resolve_fallback(SfxSoundId::kTireSkidOffRoad,
                                                                    SfxSoundId::kTireSkidOnRoad)));
        case 5:
          return resolve_fallback(SfxSoundId::kRallyTireSkidMud,
                                  resolve_fallback(SfxSoundId::kRallyTireSkidDirt,
                                                   resolve_fallback(SfxSoundId::kTireSkidOffRoad,
                                                                    SfxSoundId::kTireSkidOnRoad)));
        case 0:
          return resolve_fallback(SfxSoundId::kRallyTireSkidTarmac, SfxSoundId::kTireSkidOnRoad);
        default: break;
      }
    }
    switch (sgrp) {
      case 1: return resolve_fallback(SfxSoundId::kTireSkidOffRoad, SfxSoundId::kTireSkidOnRoad);
      case 2: return resolve_fallback(SfxSoundId::kTireSkidGrass, SfxSoundId::kTireSkidOnRoad);
      case 3: return resolve_fallback(SfxSoundId::kTireSkidBrick, SfxSoundId::kTireSkidOnRoad);
      case 4: return resolve_fallback(SfxSoundId::kTireSkidTrackway, SfxSoundId::kTireSkidOnRoad);
      case 5:
        return resolve_fallback(SfxSoundId::kRallyTireSkidMud,
                                resolve_fallback(SfxSoundId::kTireSkidOffRoad, SfxSoundId::kTireSkidOnRoad));
      default: return SfxSoundId::kTireSkidOnRoad;
    }
  };

  // 1. Wind (Bus::kWind): Wind.fev's own curves on Speed (mph) and TopSpeedPercent.
  float* const wind_bus_ptr = buses[int(Bus::kWind)];
  if (wind_event_.ok) {
    std::array<float, int(EvParam::kCount)> in{};
    in[int(EvParam::kSpeed)] = std::max(0.0f, st.speed_mph);
    in[int(EvParam::kTopSpeedPercent)] =
        st.top_speed_mph > 1.0f ? std::clamp(st.speed_mph / st.top_speed_mph, 0.0f, 1.0f) : 0.0f;
    // BoostCarWindAudio and MusicActive stay at 0: their setters are not decoded (Q333).
    if (wind_bus_ptr) mix_event(wind_event_, in, view_wind, 1.0f, 0.707f, 0.707f, wind_bus_ptr, frames);
  } else if (float* wind_bus = wind_bus_ptr) {
    const float sp = std::clamp((st.speed_mph - 12.0f) / 180.0f, 0.0f, 1.0f);
    const float music_duck = st.music_active ? 0.75f : 1.0f;
    const float gain = 0.45f * std::pow(sp, 1.35f) * music_duck * view_wind;
    const float pitch = 0.75f + 0.55f * std::clamp(st.speed_mph / 200.0f, 0.0f, 1.0f);
    mix_loop(wind_voice_, SfxSoundId::kWind, gain, pitch, 0.707f, 0.707f, wind_bus, frames);
  }

  // 2. Per-wheel 4-corner stereo-panned tyre roll/skid, distinct SurfaceIndex timbre, & SurfaceChanges.xml (Bus::kTyres)
  {
    float* tyre_bus = buses[int(Bus::kTyres)];
    static constexpr float kWheelPanL[4] = {0.84f, 0.54f, 0.80f, 0.58f};
    static constexpr float kWheelPanR[4] = {0.54f, 0.84f, 0.58f, 0.80f};
    bool tyre_events_loaded = false;
    for (const auto& g : tyre_events_) tyre_events_loaded = tyre_events_loaded || g[0][0].ok;
    for (int w = 0; w < 4; ++w) {
      const auto& wh = st.wheels[size_t(w)];
      const float pl = kWheelPanL[w];
      const float pr = kWheelPanR[w];
      const int16_t cur_sidx = wh.surface_index;
      const int16_t prev_sidx = prev_wheel_surface_idx_[size_t(w)];
      const bool sidx_changed = (cur_sidx >= 0 && prev_sidx >= 0 && cur_sidx != prev_sidx);
      const bool sgrp_changed = (wh.surface_group != prev_wheel_surface_[size_t(w)]);

      if ((sidx_changed || sgrp_changed) && st.speed_mph > 5.0f) {
        const float base_g = 0.35f * std::clamp(st.speed_mph / 106.0f, 0.1f, 1.0f) * view_tyre;
        if (sidx_changed) {
          const SurfaceChangeSpec spec = tires_cfg_.surface_change_spec_by_index(prev_sidx, cur_sidx);
          if (!spec.event.empty()) {
            const std::string ev_low = lower_str(spec.event);
            if (ev_low != "silent" && ev_low != "none") {
              SfxSoundId sc_id = SfxSoundId::kSurfaceHardToHard;
              if (ev_low.find("leaves_on") != std::string::npos || ev_low.find("leaveson") != std::string::npos) {
                sc_id = resolve_fallback(SfxSoundId::kSurfaceLeavesOn, SfxSoundId::kSurfaceDirtToHard);
              } else if (ev_low.find("leaves_off") != std::string::npos ||
                         ev_low.find("leavesoff") != std::string::npos) {
                sc_id = resolve_fallback(SfxSoundId::kSurfaceLeavesOff, SfxSoundId::kSurfaceDirtToHard);
              } else if (ev_low.find("dirt") != std::string::npos || ev_low.find("gravel") != std::string::npos ||
                         ev_low.find("grass") != std::string::npos) {
                sc_id = SfxSoundId::kSurfaceDirtToHard;
              }
              spawn_one_shot(sc_id, Bus::kTyres, base_g * std::clamp(spec.gain, 0.05f, 2.5f), pl, pr,
                             std::clamp(spec.pitch, 0.5f, 2.0f));
            }
          } else {
            const bool cur_leaves = (cur_sidx >= 51 && cur_sidx <= 55);
            const bool prev_leaves = (prev_sidx >= 51 && prev_sidx <= 55);
            if (cur_leaves && !prev_leaves) {
              spawn_one_shot(resolve_fallback(SfxSoundId::kSurfaceLeavesOn, SfxSoundId::kSurfaceDirtToHard),
                             Bus::kTyres, base_g, pl, pr);
            } else if (prev_leaves && !cur_leaves) {
              spawn_one_shot(resolve_fallback(SfxSoundId::kSurfaceLeavesOff, SfxSoundId::kSurfaceDirtToHard),
                             Bus::kTyres, base_g, pl, pr);
            } else {
              const bool dirt_edge = (wh.surface_group == 1 || wh.surface_group == 2 ||
                                      prev_wheel_surface_[size_t(w)] == 1 || prev_wheel_surface_[size_t(w)] == 2);
              spawn_one_shot(dirt_edge ? SfxSoundId::kSurfaceDirtToHard : SfxSoundId::kSurfaceHardToHard, Bus::kTyres,
                             base_g, pl, pr);
            }
          }
        } else {
          const bool dirt_edge = (wh.surface_group == 1 || wh.surface_group == 2 ||
                                  prev_wheel_surface_[size_t(w)] == 1 || prev_wheel_surface_[size_t(w)] == 2);
          spawn_one_shot(dirt_edge ? SfxSoundId::kSurfaceDirtToHard : SfxSoundId::kSurfaceHardToHard, Bus::kTyres,
                         base_g, pl, pr);
        }
      }
      prev_wheel_surface_[size_t(w)] = wh.surface_group;
      if (cur_sidx >= 0) prev_wheel_surface_idx_[size_t(w)] = cur_sidx;

      // Suspension BottomOut (Bus::kSuspension -> fallback Bus::kEngine)
      if (wh.bottom_out_force > 0.002f && (time_s_ - susp_cooldown_s_[size_t(w)]) > 0.25) {
        susp_cooldown_s_[size_t(w)] = time_s_;
        const float sg = 0.6f * std::clamp(wh.bottom_out_force / 0.07f, 0.2f, 1.0f) *
                         safe_clamp_f(st.susp_vol_scale, 0.0f, 2.5f, 1.0f) * view_susp;
        spawn_one_shot(SfxSoundId::kSuspensionBottom, buses[int(Bus::kSuspension)] ? Bus::kSuspension : Bus::kEngine,
                       sg, pl, pr);
      }

      if (tyre_bus && tyre_events_loaded && !st.rally_mode) {
        // Tires.fev LOD1: the wheel's Roll and Skid events in its surface's group,
        // fed what the game's per-wheel update sets (Q331); the other groups' voices fade out.
        static constexpr int16_t kGroupIndex[kTyreGroups] = {0, 11, 15, 2, 0};
        const int grp = wh.surface_group < kTyreGroups ? int(wh.surface_group) : 1;  // Mud plays OffRoad
        std::array<float, int(EvParam::kCount)> in{};
        in[int(EvParam::kSlipAngle)] = std::fabs(wh.slip_angle);
        in[int(EvParam::kSlipRatio)] = wh.slip_ratio;
        in[int(EvParam::kSurfaceIndex)] = float(cur_sidx >= 0 ? cur_sidx : kGroupIndex[grp]);
        in[int(EvParam::kTireRpm)] = std::fabs(wh.wheel_rpm);
        in[int(EvParam::kLoad)] = std::max(0.0f, wh.load);
        in[int(EvParam::kCombinedSlip)] = wh.slip_combined;
        for (int g = 0; g < kTyreGroups; ++g)
          for (int k = 0; k < 2; ++k)
            mix_event(tyre_events_[size_t(g)][size_t(w)][size_t(k)], in,
                      g == grp ? view_tyre * kTyreEventLevel : 0.0f, 1.0f, pl, pr, tyre_bus, frames);
      } else if (tyre_bus) {
        // Distinct SurfaceIndex character: preserve individual FH1 surface_index timbre even
        // when two surfaces share an LOD1 group (e.g. SmoothTarmac vs RoughAsphalt vs Leaves).
        float sidx_gain = 1.0f;
        float sidx_pitch = 1.0f;
        if (cur_sidx >= 0) {
          if (cur_sidx >= 51 && cur_sidx <= 55) {
            sidx_gain = 0.85f;
            sidx_pitch = 1.12f + 0.02f * float(cur_sidx - 51);
          } else {
            const int bucket = cur_sidx % 7;
            sidx_gain = 0.90f + 0.04f * float(bucket);
            sidx_pitch = 0.92f + 0.03f * float(bucket);
          }
        }
        const float load_f = std::clamp(wh.load, 0.0f, 1.4f);
        const float wheel_linear_mph = std::fabs(wh.wheel_rpm) * 0.065f;
        const float w_speed_mph = std::max(st.speed_mph, wheel_linear_mph);
        const float roll_g = 0.12f * std::clamp(w_speed_mph / 90.0f, 0.0f, 1.0f) * load_f * view_tyre * sidx_gain;
        const float roll_p =
            (0.65f + 0.75f * std::clamp(w_speed_mph / 130.0f, 0.0f, 1.0f) + 0.015f * float(w)) * sidx_pitch;
        mix_loop(tire_roll_voices_[size_t(w)], roll_for_surface(wh.surface_group), roll_g, roll_p, pl, pr, tyre_bus,
                 frames);

        // Multi-layer scrub-to-screech progression driven from 0 mph via wheel_rpm + SlipRatio/SlipAngle/Load
        // (no hard speed_mph > 4 mph gate so stationary burnouts & launch wheelspin screech authentically).
        const float slip_long = safe_clamp_f(std::fabs(wh.slip_ratio), 0.0f, 4.0f, 0.0f);
        const float slip_lat = safe_clamp_f(std::fabs(wh.slip_angle), 0.0f, 4.0f, 0.0f);
        const float eff_slip = std::max(safe_clamp_f(wh.slip_combined, 0.0f, 4.0f, 0.0f),
                                        std::hypot(slip_long, slip_lat));
        const bool loose_surface = (wh.surface_group == 1 || wh.surface_group == 2 || wh.surface_group == 5);
        const float scrub_onset = loose_surface ? 0.08f : 0.10f;
        const float scrub_t = std::clamp((eff_slip - scrub_onset) / 0.34f, 0.0f, 1.0f);
        const float scrub_env = scrub_t * scrub_t * (3.0f - 2.0f * scrub_t);  // smoothstep scrub layer
        const float screech_t = std::clamp((eff_slip - 0.32f) / 0.78f, 0.0f, 1.0f);
        const float slip_speed_gate = std::clamp(w_speed_mph / 3.5f, 0.0f, 1.0f);
        const float load_gate = std::clamp((load_f - 0.03f) / 0.12f, 0.0f, 1.0f);
        const float load_scale = std::clamp(std::sqrt(std::max(0.0f, load_f)), 0.0f, 1.25f);
        const float burnout_boost = (wheel_linear_mph > st.speed_mph + 3.0f && eff_slip > 0.30f) ? 1.15f : 1.0f;
        const float surf_skid_scale = loose_surface ? 0.88f : 1.0f;
        const float skid_g = (0.09f * scrub_env + 0.21f * screech_t) * slip_speed_gate * load_gate * load_scale *
                             burnout_boost * surf_skid_scale * view_tyre * sidx_gain;
        const float skid_p =
            ((loose_surface ? 0.72f : 0.78f) + 0.22f * scrub_env + (loose_surface ? 0.18f : 0.26f) * screech_t +
             0.08f * std::clamp(w_speed_mph / 120.0f, 0.0f, 1.0f) + 0.012f * float(w)) *
            sidx_pitch;
        mix_loop(tire_skid_voices_[size_t(w)], skid_for_surface(wh.surface_group), skid_g, skid_p, pl, pr, tyre_bus,
                 frames);
      }
    }
  }

  // 3. Dedicated subsystem buses (Transmission, Turbo, Supercharger, EngineDamage, Backfire) + FocusCar EngineLFE / EV / ExhNoise
  {
    auto bus_or = [&](Bus preferred, Bus fallback) -> float* {
      return buses[int(preferred)] ? buses[int(preferred)] : buses[int(fallback)];
    };
    auto bus_id_or = [&](Bus preferred, Bus fallback) -> Bus {
      return buses[int(preferred)] ? preferred : fallback;
    };
    const float rpm_span = std::max(500.0f, st.redline_rpm - st.idle_rpm);
    const float norm_rpm = std::clamp((st.rpm - st.idle_rpm) / rpm_span, 0.0f, 1.2f);
    const bool shift_rising = st.shifting && !prev_shifting_;
    const float cur_thr = safe_clamp_f(st.throttle, 0.0f, 1.0f, 0.0f);

    // Evaluate hard_liftoff over a 33 ms sliding throttle history window (frame-rate independent).
    float max_recent_thr = cur_thr;
    constexpr double kLiftoffWindowS = 0.0335;
    for (int i = 0; i < thr_hist_count_; ++i) {
      const int idx = (thr_hist_head_ - 1 - i + kThrottleHistSize) % kThrottleHistSize;
      const auto& entry = thr_hist_[size_t(idx)];
      const double age = prev_time_s - entry.time_s;
      if (i == 0 || (age >= -1e-6 && age <= kLiftoffWindowS)) {
        max_recent_thr = std::max(max_recent_thr, entry.throttle);
      }
    }
    if (cur_thr > 0.35f) liftoff_armed_ = true;
    const bool hard_liftoff = liftoff_armed_ && (max_recent_thr > 0.55f && cur_thr < 0.08f);
    if (hard_liftoff) liftoff_armed_ = false;
    thr_hist_[size_t(thr_hist_head_)] = {time_s_, cur_thr};
    thr_hist_head_ = (thr_hist_head_ + 1) % kThrottleHistSize;
    if (thr_hist_count_ < kThrottleHistSize) ++thr_hist_count_;

    // Transmission whine (tier-matched L0..L3) + shift start/complete L0..L3 (Bus::kTransmission).
    // Driven by gearbox shaft speed (wheel_speed_mps * gear ratio, with distinct straight-cut reverse whine)
    // and gated by clutch engagement (1 - clutch_pos) & torque load so clutching-in revving stays silent.
    if (float* trans_bus = bus_or(Bus::kTransmission, Bus::kEngine)) {
      const int tlv = std::clamp(st.trans_level, 0, 3);
      static constexpr SfxSoundId kTransTiers[4] = {SfxSoundId::kTransWhine, SfxSoundId::kTransWhineL1,
                                                    SfxSoundId::kTransWhineL2, SfxSoundId::kTransWhineL3};
      static constexpr SfxSoundId kShiftStartTiers[4] = {SfxSoundId::kShiftStart, SfxSoundId::kShiftStartL1,
                                                         SfxSoundId::kShiftStartL2, SfxSoundId::kShiftStartL3};
      static constexpr SfxSoundId kShiftDoneTiers[4] = {SfxSoundId::kShiftComplete, SfxSoundId::kShiftCompleteL1,
                                                        SfxSoundId::kShiftCompleteL2, SfxSoundId::kShiftCompleteL3};
      const SfxSoundId trans_id = resolve_fallback(kTransTiers[tlv], SfxSoundId::kTransWhine);
      const float tv_scale = safe_clamp_f(st.trans_vol_scale, 0.0f, 2.5f, 1.0f) * view_trans;
      const float sv_scale = safe_clamp_f(st.shift_vol_scale, 0.0f, 2.5f, 1.0f) * view_trans;
      const bool in_reverse = (st.gear < 0);
      const float clutch_engage = (st.gear == 0) ? 0.0f : std::clamp(1.0f - safe_clamp_f(st.clutch_pos, 0.0f, 1.0f, 0.0f), 0.0f, 1.0f);
      float avg_wheel_rpm = 0.0f;
      int spinning_wheels = 0;
      for (const auto& wh : st.wheels) {
        const float aw = std::fabs(wh.wheel_rpm);
        if (aw > 1.0f) {
          avg_wheel_rpm += aw;
          ++spinning_wheels;
        }
      }
      if (spinning_wheels > 0) avg_wheel_rpm /= float(spinning_wheels);
      const float wheel_mps_from_rpm = avg_wheel_rpm * 0.03456f;  // ~0.33 m radius
      const float wheel_mps = std::max(std::max(0.0f, st.speed_mph) * 0.44704f, wheel_mps_from_rpm);
      float eff_ratio = safe_clamp_f(st.gear_ratio, 0.0f, 30.0f, 0.0f);
      if (eff_ratio <= 0.1f) {
        if (in_reverse) {
          eff_ratio = 3.40f * 3.70f;
        } else if (st.gear >= 1) {
          static constexpr float kDefaultGears[8] = {3.30f, 2.13f, 1.55f, 1.21f, 1.00f, 0.82f, 0.68f, 0.58f};
          eff_ratio = kDefaultGears[std::clamp(st.gear - 1, 0, 7)] * 3.70f;
        }
      }
      const float shaft_rpm = (wheel_mps / (2.0f * 3.14159265f * 0.33f)) * 60.0f * eff_ratio;
      const float shaft_norm = std::clamp(shaft_rpm / std::max(2500.0f, st.redline_rpm), 0.0f, 1.35f);
      const float speed_gate = std::clamp(wheel_mps / 1.5f, 0.0f, 1.0f);
      const float torque_load = std::clamp(std::max(safe_clamp_f(st.engine_load, 0.0f, 1.0f, 0.0f), cur_thr), 0.0f, 1.0f);
      const float load_mod = in_reverse ? (0.65f + 0.35f * torque_load) : (0.55f + 0.45f * torque_load);
      const float tier_base = in_reverse ? std::max(0.14f, 0.08f + 0.05f * float(tlv)) : (0.05f + 0.065f * float(tlv));
      const float trans_g = tier_base * tv_scale * std::clamp(shaft_norm, 0.15f, 1.0f) * clutch_engage * speed_gate * load_mod;
      const float trans_p = in_reverse ? (0.85f + 1.45f * std::clamp(wheel_mps / 18.0f, 0.0f, 1.2f))
                                       : (0.50f + 1.25f * shaft_norm);
      mix_loop(trans_voice_, trans_id, trans_g, trans_p, 0.707f, 0.707f, trans_bus, frames);
      const Bus tbus = bus_id_or(Bus::kTransmission, Bus::kEngine);
      if (shift_rising) {
        spawn_one_shot(resolve_fallback(kShiftStartTiers[tlv], SfxSoundId::kShiftStart), tbus, 0.35f * sv_scale,
                       0.707f, 0.707f);
        if (focus_engine) focus_engine->trigger_shift_boost(st.gear >= prev_gear_);
      }
      if (!st.shifting && prev_shifting_)
        spawn_one_shot(resolve_fallback(kShiftDoneTiers[tlv], SfxSoundId::kShiftComplete), tbus, 0.30f * sv_scale,
                       0.707f, 0.707f);
      prev_shifting_ = st.shifting;
      if (!st.shifting) prev_gear_ = st.gear;
    }

    // Turbo spool & BOV (0=none, 1=L1, 2=L2, 3=L3; 4.5 / 13.0 PSI thresholds;
    // honors per-car blowoffL1..L3Level & blowoffL1..L3Choice) (Bus::kTurbo).
    if (float* turbo_bus = bus_or(Bus::kTurbo, Bus::kEngine)) {
      const bool turbo_active = (st.forced_induction == 1 && st.turbo_level >= 1);
      const int tblv = turbo_active ? (std::clamp(st.turbo_level, 1, 3) - 1) : 0;
      static constexpr SfxSoundId kTurboTiers[3] = {SfxSoundId::kTurboSpool, SfxSoundId::kTurboSpoolL2,
                                                    SfxSoundId::kTurboSpoolL3};
      const SfxSoundId turbo_id = resolve_fallback(kTurboTiers[tblv], SfxSoundId::kTurboSpool);
      const float tb_scale = safe_clamp_f(st.turbo_vol_scale, 0.0f, 2.5f, 1.0f) * view_turbo;
      const float turbo_g = turbo_active ? 0.35f * tb_scale * std::clamp(st.turbo_norm_rpm, 0.0f, 1.0f) : 0.0f;
      const float turbo_p = 0.60f + 0.95f * std::clamp(st.turbo_norm_rpm, 0.0f, 1.2f);
      mix_loop(turbo_voice_, turbo_id, turbo_g, turbo_p, 0.707f, 0.707f, turbo_bus, frames);
      if (turbo_active && st.bov_open && !prev_bov_open_) {
        const BovVariant v = select_bov_variant(st.turbo_psi);
        if (v != BovVariant::kNone) {
          const Bus tbus = bus_id_or(Bus::kTurbo, Bus::kEngine);
          const bool hi = (v == BovVariant::kHi);
          const int bov_lvl = std::clamp(st.blowoff_level[size_t(tblv)], 1, 3);
          const int bov_choice = std::clamp(st.blowoff_choice[size_t(tblv)], 1, 5);
          const SfxSoundId want = turbo_bov_sound_id(bov_lvl, bov_choice, hi);
          const SfxSoundId fb_c1 = turbo_bov_sound_id(bov_lvl, 1, hi);
          const SfxSoundId fb_l1 = hi ? SfxSoundId::kTurboBovHi : SfxSoundId::kTurboBovLo;
          const SfxSoundId picked = resolve_fallback(want, resolve_fallback(fb_c1, fb_l1));
          spawn_one_shot(picked, tbus, (hi ? 0.55f : 0.45f) * tb_scale, 0.707f, 0.707f);
        }
      }
      prev_bov_open_ = st.bov_open;
    }

    // Supercharger whine (0=none, 1=L1, 2=L2, 3=L3; Roots/TwinScrew or Centrifugal CSC) (Bus::kSupercharger).
    if (float* sc_bus = bus_or(Bus::kSupercharger, Bus::kEngine)) {
      const bool sc_active = (st.forced_induction == 2 && st.sc_level >= 1);
      const int sclv = sc_active ? (std::clamp(st.sc_level, 1, 3) - 1) : 0;
      static constexpr SfxSoundId kRootsTiers[3] = {SfxSoundId::kScWhine, SfxSoundId::kScWhineL2, SfxSoundId::kScWhineL3};
      static constexpr SfxSoundId kCscTiers[3] = {SfxSoundId::kScCscL1, SfxSoundId::kScCscL2, SfxSoundId::kScCscL3};
      const SfxSoundId sc_id = resolve_fallback(st.sc_is_centrifugal ? kCscTiers[sclv] : kRootsTiers[sclv],
                                                SfxSoundId::kScWhine);
      const float sc_scale = safe_clamp_f(st.sc_vol_scale, 0.0f, 2.5f, 1.0f) * view_sc;
      const float sc_g = sc_active
                             ? 0.40f * sc_scale * std::clamp(st.supercharger_norm_rpm, 0.0f, 1.0f) *
                                   (0.4f + 0.6f * st.throttle)
                             : 0.0f;
      const float sc_p = 0.55f + 1.10f * std::clamp(st.supercharger_norm_rpm, 0.0f, 1.3f);
      mix_loop(sc_voice_, sc_id, sc_g, sc_p, 0.707f, 0.707f, sc_bus, frames);
    }

    // EngineLFE low-frequency bass bed (octave-law pitch; routed exclusively into mono `lfe_out`
    // when provided so LFE bypasses stereo L/R master compression).
    if (lfe_out != nullptr || buses[int(Bus::kEngine)] != nullptr) {
      SfxSoundId lfe_id = SfxSoundId::kEngineLfeLoop;
      if (st.num_cylinders >= 12) lfe_id = resolve_fallback(SfxSoundId::kEngineLfe12, lfe_id);
      else if (st.num_cylinders >= 10) lfe_id = resolve_fallback(SfxSoundId::kEngineLfe10, lfe_id);
      else if (st.num_cylinders >= 8) lfe_id = resolve_fallback(SfxSoundId::kEngineLfe8, lfe_id);
      else if (st.num_cylinders >= 6) lfe_id = resolve_fallback(SfxSoundId::kEngineLfe6, lfe_id);
      else if (st.num_cylinders >= 4) lfe_id = resolve_fallback(SfxSoundId::kEngineLfe4, lfe_id);
      const float lfe_g =
          st.is_electric
              ? 0.0f
              : 0.22f * view_lfe * std::clamp(0.25f + 0.75f * cur_thr, 0.0f, 1.0f) *
                    std::clamp(0.35f + 0.65f * norm_rpm, 0.0f, 1.0f);
      // Authentic octave-law pitch anchored at 4000 RPM: pow(2, log2(rpm / 4000) * scale + offset).
      const float rpm_ref_ratio = std::clamp(std::max(300.0f, st.rpm) / 4000.0f, 0.075f, 4.0f);
      const float lfe_p = std::clamp(std::pow(2.0f, std::log2(rpm_ref_ratio) * 0.80f), 0.35f, 2.4f);
      if (lfe_out != nullptr) {
        mix_loop_mono(engine_lfe_voice_, lfe_id, lfe_g, lfe_p, lfe_out, frames);
      } else {
        mix_loop(engine_lfe_voice_, lfe_id, lfe_g, lfe_p, 0.707f, 0.707f, buses[int(Bus::kEngine)], frames);
      }
    }

    // ExhNoise high-RPM gas rush & multi-band ElectricEngine.fev (Bus::kEngine).
    if (float* eng_bus = buses[int(Bus::kEngine)]) {
      // ExhNoise.fev: high-RPM exhaust gas/air-rush layer on ICE cars (uses view_exh_noise directly).
      const float exh_scale = safe_clamp_f(st.exh_noise_vol_scale, 0.0f, 2.5f, 1.0f) * view_exh_noise;
      const float exh_rpm = std::clamp((norm_rpm - 0.25f) / 0.75f, 0.0f, 1.0f);
      const float exh_g = st.is_electric ? 0.0f
                                         : 0.22f * exh_scale * std::pow(exh_rpm, 1.35f) *
                                               (0.35f + 0.65f * cur_thr);
      const float exh_p = 0.72f + 0.85f * std::clamp(norm_rpm, 0.0f, 1.2f);
      mix_loop(exh_noise_voice_, SfxSoundId::kExhNoise, exh_g, exh_p, 0.707f, 0.707f, eng_bus, frames);

      // Multi-band ElectricEngine.fev: 3 harmonic pitch layers (low fundamental, mid gearset, high inverter whine).
      const float ev_scale = safe_clamp_f(st.ev_vol_scale, 0.0f, 2.5f, 1.0f) * view_ev;
      const float ev_drive = std::clamp(std::max(st.speed_mph / 95.0f, norm_rpm), 0.0f, 1.25f);
      const float ev_load = 0.28f + 0.72f * cur_thr;
      const float ev_lo_g =
          st.is_electric ? (0.28f * ev_scale * ev_load * std::clamp(0.22f + 0.78f * ev_drive, 0.0f, 1.0f)) : 0.0f;
      const float ev_mid_g =
          st.is_electric ? (0.20f * ev_scale * ev_load * std::clamp((ev_drive - 0.12f) / 0.75f, 0.0f, 1.0f)) : 0.0f;
      const float ev_hi_g =
          st.is_electric ? (0.16f * ev_scale * (0.25f + 0.75f * cur_thr) *
                            std::clamp((ev_drive - 0.25f) / 0.80f, 0.0f, 1.0f))
                         : 0.0f;
      const float ev_lo_p = 0.55f + 1.35f * ev_drive;
      const float ev_mid_p = 0.78f + 1.65f * ev_drive;
      const float ev_hi_p = 1.05f + 1.95f * ev_drive;
      const SfxSoundId ev_mid_id = resolve_fallback(SfxSoundId::kElectricMotorMid, SfxSoundId::kElectricMotor);
      const SfxSoundId ev_hi_id = resolve_fallback(SfxSoundId::kElectricMotorHigh, SfxSoundId::kElectricMotor);
      const float mid_p_mult = (ev_mid_id == SfxSoundId::kElectricMotor) ? 1.50f : 1.0f;
      const float hi_p_mult = (ev_hi_id == SfxSoundId::kElectricMotor) ? 2.25f : 1.0f;
      mix_loop(electric_voice_, SfxSoundId::kElectricMotor, ev_lo_g, ev_lo_p, 0.707f, 0.707f, eng_bus, frames);
      mix_loop(electric_mid_voice_, ev_mid_id, ev_mid_g, ev_mid_p * mid_p_mult, 0.707f, 0.707f, eng_bus, frames);
      mix_loop(electric_high_voice_, ev_hi_id, ev_hi_g, ev_hi_p * hi_p_mult, 0.707f, 0.707f, eng_bus, frames);
    }

    // Engine damage loop on Bus::kEngineDamage: Damage.fev DamageEngine evaluated at
    // Damage / NormRPM / Throttle (its instance crossfade and volume envelopes).
    if (float* dmg_bus = bus_or(Bus::kEngineDamage, Bus::kEngine)) {
      if (damage_event_ok_) {
        std::array<float, 8> raw{};
        raw.fill(std::numeric_limits<float>::quiet_NaN());
        auto put = [&](int idx, float v) {
          if (idx >= 0 && size_t(idx) < raw.size()) raw[size_t(idx)] = v;
        };
        const bool running = !st.is_electric && st.rpm > 200.0f;
        put(damage_param_damage_, running ? st.engine_damage : 0.0f);
        put(damage_param_rpm_, std::clamp(norm_rpm, 0.0f, 1.0f));
        put(damage_param_throttle_, std::clamp(st.throttle, 0.0f, 1.0f));
        std::array<FevInstanceLevel, kDamageInstances> lv{};
        const int n = evaluate_fev_event(damage_event_, raw.data(), raw.size(), lv.data(), kDamageInstances);
        LoopVoice* voices[kDamageInstances] = {&damage_voice_, &damage_fast_voice_};
        for (int i = 0; i < kDamageInstances; ++i) {
          const SfxSoundId id = damage_sound_ids_[size_t(i)];
          if (id == SfxSoundId::kCount) continue;
          const float g = (i < n ? lv[size_t(i)].gain : 0.0f) * view_damage;
          // Pitch envelope scale is undecoded: the loops play at their recorded pitch.
          mix_loop(*voices[i], id, g, 1.0f, 0.707f, 0.707f, dmg_bus, frames);
        }
      } else if (procedural_mode_) {
      // No-assets stand-in only (populate_procedural_fallbacks): own gain law.
      const float dmg = std::clamp(std::max(st.engine_damage, st.drivetrain_damage * 0.7f), 0.0f, 1.0f);
      const bool dmg_active = (!st.is_electric && dmg > 0.02f && st.rpm > 200.0f);
      const float dmg_g = dmg_active ? (0.48f * dmg * view_damage * (0.35f + 0.65f * std::clamp(norm_rpm, 0.0f, 1.0f)))
                                     : 0.0f;
      const float dmg_p = 0.65f + 0.90f * std::clamp(norm_rpm, 0.0f, 1.2f);
      const PcmSample* dmg_pcm = samples_[size_t(SfxSoundId::kEngineDamageLoop)];
      if (dmg_pcm && !dmg_pcm->data.empty()) {
        mix_loop(damage_voice_, SfxSoundId::kEngineDamageLoop, dmg_g, dmg_p, 0.707f, 0.707f, dmg_bus, frames);
      } else if (dmg_active || damage_voice_.gain > 1e-4f) {
        // Real-time procedural rod-knock + metallic rattle fallback when Damage.fev bank is absent.
        constexpr double kTwoPi = 6.283185307179586;
        const double crank_hz = std::clamp(double(st.rpm) / 60.0 * 0.5, 8.0, 140.0);
        const double inv_rate = 1.0 / double(rate_);
        for (int f = 0; f < frames; ++f) {
          damage_voice_.gain += (dmg_g - damage_voice_.gain) * smooth_;
          damage_synth_phase_ += kTwoPi * crank_hz * inv_rate;
          if (damage_synth_phase_ > kTwoPi) damage_synth_phase_ -= kTwoPi;
          damage_synth_rng_ ^= damage_synth_rng_ << 13;
          damage_synth_rng_ ^= damage_synth_rng_ >> 17;
          damage_synth_rng_ ^= damage_synth_rng_ << 5;
          const float noise = float(int32_t(damage_synth_rng_)) * (1.0f / 2147483648.0f);
          damage_synth_lp_ += (noise - damage_synth_lp_) * 0.22f;
          const float knock = std::pow(std::max(0.0f, std::sin(float(damage_synth_phase_))), 6.0f);
          const float sig = (0.65f * knock * std::sin(float(damage_synth_phase_ * 7.0)) +
                             0.35f * (noise - damage_synth_lp_) * (0.4f + 0.6f * knock)) *
                            damage_voice_.gain;
          dmg_bus[2 * f] += sig * 0.707f;
          dmg_bus[2 * f + 1] += sig * 0.707f;
        }
      }
      }
    }

    // Distinct off-throttle overrun <Burble> crackles vs shift/decel <Backfire> pops (Bus::kBackfire).
    if (bus_or(Bus::kBackfire, Bus::kEngine) || lfe_out != nullptr) {
      const int bf_lv = std::clamp(st.backfire_level, 0, 3);
      const Bus bbus = bus_id_or(Bus::kBackfire, Bus::kEngine);
      auto pick_from_pool = [&](const SfxSoundId (&pool)[4], SfxSoundId fallback) -> SfxSoundId {
        SfxSoundId avail[4];
        int navail = 0;
        for (SfxSoundId bid : pool) {
          const auto* s = samples_[size_t(bid)];
          if (s && !s->data.empty()) avail[navail++] = bid;
        }
        return navail > 0 ? avail[std::uniform_int_distribution<int>(0, navail - 1)(rng_)] : fallback;
      };
      static constexpr SfxSoundId kBackfirePool[4] = {SfxSoundId::kBackfirePop, SfxSoundId::kBackfirePop1,
                                                      SfxSoundId::kBackfirePop2, SfxSoundId::kBackfirePop3};
      static constexpr SfxSoundId kBurblePool[4] = {SfxSoundId::kBurblePop0, SfxSoundId::kBurblePop1,
                                                    SfxSoundId::kBurblePop2, SfxSoundId::kBurblePop3};
      const float bf_scale = safe_clamp_f(st.backfire_vol_scale, 0.0f, 2.5f, 1.0f) * view_backfire;
      const float raw_bu_vol =
          (st.burble_vol_scale > 0.0f) ? st.burble_vol_scale : (bf_lv > 0 ? st.backfire_vol_scale : 0.0f);
      const float bu_scale = safe_clamp_f(raw_bu_vol, 0.0f, 2.5f, 0.0f) * view_burble;

      if (bf_lv > 0 && !st.is_electric) {
        const auto& lv = backfire_cfg_.levels[size_t(bf_lv)];
        // Shift / hard throttle lift-off <Backfire> pop.
        if ((shift_rising && norm_rpm >= 0.42f) || (hard_liftoff && norm_rpm >= 0.48f)) {
          const SfxSoundId pop_id = pick_from_pool(kBackfirePool, SfxSoundId::kBackfirePop);
          const float bg =
              0.68f * bf_scale * float(lv.volume.present ? lv.volume.eval({norm_rpm, 0, 0, 1}) : 0.8);
          const float bpitch = 0.90f + 0.18f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
          spawn_one_shot(pop_id, bbus, bg, 0.707f, 0.707f, bpitch);
          spawn_one_shot(SfxSoundId::kBackfireLfe, bbus, bg * 0.70f, 0.707f, 0.707f);
          backfire_count_.fetch_add(1, std::memory_order_relaxed);
          if (focus_engine) focus_engine->trigger_expander();
        }

        // Decel <Backfire> bangs governed by BackfireTimerConfig.xml.
        if (cur_thr < 0.05f && norm_rpm >= float(lv.min_norm_rpm) && norm_rpm <= float(lv.max_norm_rpm) &&
            lv.max_time_ms > 0.0) {
          backfire_timer_ms_ -= dt * 1000.0;
          if (backfire_timer_ms_ <= 0.0) {
            std::uniform_real_distribution<double> tdist(std::max(50.0, lv.min_time_ms),
                                                         std::max(lv.min_time_ms + 10.0, lv.max_time_ms));
            backfire_timer_ms_ = tdist(rng_);
            std::uniform_real_distribution<double> sdist(0.0, 100.0);
            if (sdist(rng_) >= lv.silence_weighting) {
              const SfxSoundId pop_id = pick_from_pool(kBackfirePool, SfxSoundId::kBackfirePop);
              const float bg =
                  0.60f * bf_scale * float(lv.volume.present ? lv.volume.eval({norm_rpm, 0, 0, 1}) : 0.7);
              const float bpitch = 0.92f + 0.16f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
              spawn_one_shot(pop_id, bbus, bg, 0.707f, 0.707f, bpitch);
              spawn_one_shot(SfxSoundId::kBackfireLfe, bbus, bg * 0.65f, 0.707f, 0.707f);
              backfire_count_.fetch_add(1, std::memory_order_relaxed);
              if (focus_engine) focus_engine->trigger_expander();
            }
          }
        }
      }

      // Off-throttle overrun <Burble> crackle (decoupled from backfire_level > 0 so stock sports cars
      // with <Burble> tunings crackle authentically and trigger focus_engine->trigger_expander()).
      if (!st.is_electric && cur_thr < 0.05f && bu_scale > 1e-4f && st.burble_max_time_ms > 0.0f) {
        const float bu_min_rpm = std::clamp(st.burble_min_norm_rpm, 0.05f, 0.95f);
        const float bu_max_rpm = std::clamp(st.burble_max_norm_rpm, bu_min_rpm + 0.05f, 1.25f);
        if (norm_rpm >= bu_min_rpm && norm_rpm <= bu_max_rpm) {
          burble_timer_ms_ -= dt * 1000.0;
          if (burble_timer_ms_ <= 0.0) {
            const double min_ms = std::max(40.0, double(st.burble_min_time_ms));
            const double max_ms = std::max(min_ms + 15.0, double(st.burble_max_time_ms));
            burble_timer_ms_ = std::uniform_real_distribution<double>(min_ms, max_ms)(rng_);
            if (std::uniform_real_distribution<double>(0.0, 100.0)(rng_) >= double(st.burble_silence_weight)) {
              const SfxSoundId bu_id =
                  pick_from_pool(kBurblePool, pick_from_pool(kBackfirePool, SfxSoundId::kBackfirePop));
              const float bug = 0.36f * bu_scale * std::clamp(norm_rpm, 0.25f, 1.0f);
              const float bup = 0.96f + 0.22f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
              spawn_one_shot(bu_id, bbus, bug, 0.707f, 0.707f, bup);
              burble_count_.fetch_add(1, std::memory_order_relaxed);
              if (focus_engine) focus_engine->trigger_expander();
            }
          }
        }
      }
    }

    // ActiveAero.fev wing / airbrake deployment servo cue (Bus::kWind).
    if (std::fabs(st.active_aero_deploy - prev_active_aero_) > 0.25f && (time_s_ - aero_cooldown_s_) > 0.6) {
      aero_cooldown_s_ = time_s_;
      const float ag = 0.36f * std::clamp(st.speed_mph / 110.0f, 0.35f, 1.0f) * view_wind;
      const float ap = st.active_aero_deploy > prev_active_aero_ ? 1.05f : 0.88f;
      spawn_one_shot(SfxSoundId::kActiveAero, bus_id_or(Bus::kWind, Bus::kEngine), ag, 0.707f, 0.707f, ap);
      prev_active_aero_ = st.active_aero_deploy;
    }

    // Reflections.fev canyon / guardrail slap-back reflection bed (Bus::kAmbience; uses view_refl directly).
    if (float* refl_bus = bus_or(Bus::kAmbience, Bus::kWind)) {
      const float prox = std::clamp(st.reflection_proximity, 0.0f, 1.0f);
      const float refl_g = (prox > 0.05f && st.speed_mph > 15.0f)
                               ? 0.26f * prox * std::clamp((st.speed_mph - 15.0f) / 95.0f, 0.0f, 1.0f) *
                                     (0.45f + 0.55f * cur_thr) * view_refl
                               : 0.0f;
      const float refl_p = 0.75f + 0.55f * std::clamp(st.speed_mph / 120.0f, 0.0f, 1.1f);
      mix_loop(reflection_voice_, SfxSoundId::kRoadsideReflection, refl_g, refl_p, 0.707f, 0.707f, refl_bus, frames);
    }
  }

  // 3b. Glass-break transition events (all 5 Glass.fev variants), surface-specific Scrape3D loop, & Rewind Mulligan loop.
  {
    const uint8_t newly_broken = st.glass_broken_mask & static_cast<uint8_t>(~prev_glass_broken_mask_);
    if (newly_broken != 0u) {
      static constexpr SfxSoundId kGlassIds[5] = {SfxSoundId::kGlassShatter, SfxSoundId::kGlassSideWindow,
                                                  SfxSoundId::kGlassRearWindow, SfxSoundId::kGlassTaillight,
                                                  SfxSoundId::kGlassLexan};
      for (int bit = 0; bit < 5; ++bit) {
        if ((newly_broken & (1u << bit)) == 0u) continue;
        const SfxSoundId gid = resolve_fallback(kGlassIds[bit], SfxSoundId::kGlassShatter);
        const float gp = 0.94f + 0.12f * float(std::uniform_real_distribution<double>(0.0, 1.0)(rng_));
        spawn_one_shot(gid, Bus::kCollision, 0.62f * view_amb, 0.707f, 0.707f, gp);
      }
    }
    prev_glass_broken_mask_ = st.glass_broken_mask;
  }
  if (float* col_bus = buses[int(Bus::kCollision)] ? buses[int(Bus::kCollision)] : buses[int(Bus::kAmbience)]) {
    SfxSoundId scrape_id = SfxSoundId::kScrapeLoop;
    if (st.scrape_surface_hint == 2) scrape_id = resolve_fallback(SfxSoundId::kScrapeDirt, SfxSoundId::kScrapeLoop);
    else if (st.scrape_surface_hint == 3) scrape_id = resolve_fallback(SfxSoundId::kScrapeMetal, SfxSoundId::kScrapeLoop);
    else if (st.scrape_surface_hint == 4) scrape_id = resolve_fallback(SfxSoundId::kScrapeWood, SfxSoundId::kScrapeLoop);
    const float sc_g = st.scraping ? std::clamp(st.scrape_intensity, 0.15f, 1.0f) * 0.55f * view_amb : 0.0f;
    const float sc_p = 0.75f + 0.45f * std::clamp(st.speed_mph / 80.0f, 0.0f, 1.0f);
    mix_loop(scrape_voice_, scrape_id, sc_g, sc_p, 0.707f, 0.707f, col_bus, frames);
  }
  if (float* ui_bus = buses[int(Bus::kUi)]) {
    if (st.rewinding) {
      rewind_elapsed_s_ += dt;
      const float rw_spd = (st.rewind_speed > 0.01f) ? safe_clamp_f(st.rewind_speed, 0.25f, 3.0f, 1.0f) : 1.0f;
      const float rw_prog = (st.rewind_progress > 0.0f)
                                ? safe_clamp_f(st.rewind_progress, 0.0f, 1.0f, 0.0f)
                                : std::clamp(float(rewind_elapsed_s_ / 3.0), 0.0f, 1.0f);
      const float spin_env = std::clamp(float(rewind_elapsed_s_ / 0.12), 0.35f, 1.0f);
      const float rw_g =
          0.55f * std::clamp(0.45f + 0.55f * rw_spd, 0.35f, 1.15f) * (0.85f + 0.20f * rw_prog) * spin_env;
      const float rw_p =
          std::clamp((0.72f + 0.32f * rw_spd) * (0.94f + 0.18f * rw_prog) * (0.85f + 0.15f * spin_env), 0.50f, 2.0f);
      mix_loop(rewind_voice_, SfxSoundId::kRewindLoop, rw_g, rw_p, 0.707f, 0.707f, ui_bus, frames);
    } else {
      rewind_elapsed_s_ = 0.0;
      mix_loop(rewind_voice_, SfxSoundId::kRewindLoop, 0.0f, rewind_voice_.pitch, 0.707f, 0.707f, ui_bus, frames);
    }
  }
  // Hub / Homespace ambience (Autoshow/Paintshop/Workshop) routes to Bus::kHomespaceAmbience
  // (fallback Bus::kAmbience) so hub ambience is not erroneously ducked by -6 dB in UIExterior.
  if (float* hub_bus =
          buses[int(Bus::kHomespaceAmbience)] ? buses[int(Bus::kHomespaceAmbience)] : buses[int(Bus::kAmbience)]) {
    SfxSoundId hub_id = SfxSoundId::kHubAmbAutoshow;
    float hub_g = 0.0f;
    if (st.hub_ambience == 1) {
      hub_id = SfxSoundId::kHubAmbAutoshow;
      hub_g = 0.24f;
    } else if (st.hub_ambience == 2) {
      hub_id = SfxSoundId::kHubAmbPaintshop;
      hub_g = 0.24f;
    } else if (st.hub_ambience == 3) {
      hub_id = SfxSoundId::kHubAmbWorkshop;
      hub_g = 0.24f;
    }
    mix_loop(hub_amb_voice_, hub_id, hub_g, 1.0f, 0.707f, 0.707f, hub_bus, frames);
  }

  // 4. Persistent 3D spatialized AI opponent (archetype 2-band RPM pool) & traffic car engines + near-miss Whoosh3D (Bus::kOtherCars)
  if (float* other_bus = buses[int(Bus::kOtherCars)]) {
    static constexpr SfxSoundId kOppLoBanks[4] = {SfxSoundId::kOtherCarLoop, SfxSoundId::kOtherCarLoop1,
                                                  SfxSoundId::kOtherCarLoop2, SfxSoundId::kOtherCarLoop3};
    static constexpr SfxSoundId kOppHiBanks[4] = {SfxSoundId::kOtherCarLoopHi, SfxSoundId::kOtherCarLoop1Hi,
                                                  SfxSoundId::kOtherCarLoop2Hi, SfxSoundId::kOtherCarLoop3Hi};
    const Vec3f lp = clamp_audio_position(st.listener_pos);
    constexpr int kMaxSlots = VehicleAudioState::kMaxOtherCars;
    std::array<uint32_t, kMaxSlots> eff_ids{};
    std::array<float, kMaxSlots> car_dists{};
    std::array<int, kMaxSlots> matched_slot{};
    std::array<int, kMaxSlots> slot_car{};
    matched_slot.fill(-1);
    slot_car.fill(-1);

    for (int c = 0; c < kMaxSlots; ++c) {
      const auto& car = st.other_cars[size_t(c)];
      if (!car.active) continue;
      eff_ids[size_t(c)] = (car.car_id != 0u) ? car.car_id : (0xF0000000u | uint32_t(c));
      const Vec3f cp = clamp_audio_position(car.pos);
      const float dx = cp.x - lp.x, dy = cp.y - lp.y, dz = cp.z - lp.z;
      car_dists[size_t(c)] = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Pass 1: Keep existing persistent bindings by car_id (zero voice swaps on distance-sort crossings).
    for (int c = 0; c < kMaxSlots; ++c) {
      if (eff_ids[size_t(c)] == 0u) continue;
      for (int v = 0; v < kMaxSlots; ++v) {
        if (slot_car[size_t(v)] >= 0) continue;
        if (other_slots_[size_t(v)].bound_car_id == eff_ids[size_t(c)]) {
          matched_slot[size_t(c)] = v;
          slot_car[size_t(v)] = c;
          other_slots_[size_t(v)].fading_out = false;
          other_slots_[size_t(v)].fade_out_rem_s = 0.0f;
          other_slots_[size_t(v)].last_dist_m = car_dists[size_t(c)];
          break;
        }
      }
    }

    // Pass 2: Bind unmatched incoming cars into completely free voice slots.
    for (int c = 0; c < kMaxSlots; ++c) {
      if (eff_ids[size_t(c)] == 0u || matched_slot[size_t(c)] >= 0) continue;
      for (int v = 0; v < kMaxSlots; ++v) {
        auto& sl = other_slots_[size_t(v)];
        if (sl.bound_car_id == 0u && !sl.fading_out && slot_car[size_t(v)] < 0) {
          sl.bound_car_id = eff_ids[size_t(c)];
          sl.active = true;
          sl.fading_out = false;
          sl.fade_out_rem_s = 0.0f;
          sl.last_dist_m = car_dists[size_t(c)];
          sl.lo_voice = {};
          sl.hi_voice = {};
          sl.tire_voice = {};
          matched_slot[size_t(c)] = v;
          slot_car[size_t(v)] = c;
          break;
        }
      }
    }

    // Pass 3: Distance hysteresis (6 m) — if all 6 slots are busy, initiate a clean 25 ms fade-out
    // on the farthest active slot only when a closer car beats it by > 6 m (never hard-snapping mid-frame).
    constexpr float kVoiceHysteresisM = 6.0f;
    for (int c = 0; c < kMaxSlots; ++c) {
      if (eff_ids[size_t(c)] == 0u || matched_slot[size_t(c)] >= 0) continue;
      int far_v = -1;
      float far_d = car_dists[size_t(c)] + kVoiceHysteresisM;
      for (int v = 0; v < kMaxSlots; ++v) {
        if (slot_car[size_t(v)] < 0) continue;
        if (other_slots_[size_t(v)].last_dist_m > far_d) {
          far_d = other_slots_[size_t(v)].last_dist_m;
          far_v = v;
        }
      }
      if (far_v >= 0) {
        slot_car[size_t(far_v)] = -1;
        other_slots_[size_t(far_v)].fading_out = true;
        other_slots_[size_t(far_v)].fade_out_rem_s = 0.025f;
      }
    }

    // Pass 4: Render each persistent voice slot (active cars or 25 ms fade-out before reuse).
    for (int v = 0; v < kMaxSlots; ++v) {
      auto& sl = other_slots_[size_t(v)];
      const int c = slot_car[size_t(v)];
      if (c < 0) {
        if (sl.bound_car_id != 0u) {
          if (!sl.fading_out) {
            sl.fading_out = true;
            sl.fade_out_rem_s = 0.025f;
          }
          const float saved_smooth = smooth_;
          // Fast ~5 ms time constant ensures clean attenuation to silence within the 25 ms fade window.
          smooth_ = float(1.0 - std::exp(-1.0 / (0.0045 * double(rate_))));
          if (sl.lo_voice.gain > 1e-4f) {
            mix_loop(sl.lo_voice, sl.last_lo_id, 0.0f, sl.lo_voice.pitch, sl.lo_voice.left, sl.lo_voice.right,
                     other_bus, frames);
          } else {
            sl.lo_voice.gain = 0.0f;
          }
          if (sl.last_hi_id != SfxSoundId::kCount && sl.hi_voice.gain > 1e-4f) {
            mix_loop(sl.hi_voice, sl.last_hi_id, 0.0f, sl.hi_voice.pitch, sl.hi_voice.left, sl.hi_voice.right,
                     other_bus, frames);
          } else {
            sl.hi_voice.gain = 0.0f;
          }
          if (sl.tire_voice.gain > 1e-4f) {
            mix_loop(sl.tire_voice, sl.last_tire_id, 0.0f, sl.tire_voice.pitch, sl.tire_voice.left, sl.tire_voice.right,
                     other_bus, frames);
          } else {
            sl.tire_voice.gain = 0.0f;
          }
          if (EnginePlayer* ep = other_engine_for(sl.bound_car_id); ep && sl.eng_gain > 1e-5f) {
            mix_other_engine(sl, *ep, 0.0f, sl.eng_left, sl.eng_right, other_bus, frames);
          } else {
            sl.eng_gain = 0.0f;
          }
          smooth_ = saved_smooth;
          sl.fade_out_rem_s -= float(dt);
          if (sl.fade_out_rem_s <= 0.0f) {
            sl.bound_car_id = 0u;
            sl.active = false;
            sl.fading_out = false;
            sl.fade_out_rem_s = 0.0f;
            sl.lo_voice.gain = 0.0f;
            sl.hi_voice.gain = 0.0f;
            sl.tire_voice.gain = 0.0f;
          }
        }
        continue;
      }

      const auto& car = st.other_cars[size_t(c)];
      const float dist = car_dists[size_t(c)];
      const RolloffConfig& rc = (car.is_traffic || car.archetype == 6) ? traffic_rolloff_ : car_rolloff_;
      const float fov_scale = safe_clamp_f(car.fov_rolloff_scale, 0.25f, 4.0f, 1.0f);
      const float atten = rc.chassis_exhaust_l.eval(dist, fov_scale);
      const float doppler = compute_doppler_pitch(st.listener_pos, st.listener_vel, car.pos, car.vel, 1.0f);
      float pl = 0.707f, pr = 0.707f;
      compute_stereo_pan(st.listener_pos, st.listener_right, car.pos, pl, pr);
      const float p_scale = safe_clamp_f(car.pitch_scale, 0.70f, 1.40f, 1.0f);

      // The car's own engine sound, heard at the Default view through the
      // car's chassis rolloff, placed and Doppler-shifted as a point source.
      EnginePlayer* own = other_engine_for(eff_ids[size_t(c)]);
      const bool owned_mode = other_engines_owned_.load(std::memory_order_acquire);
      bool mix_shared_loops = false;
      if (own || (owned_mode && car.archetype != 5)) {
        if (own) {
          EnginePlayer::Update u;
          u.rpm = car.rpm;
          u.throttle = car.throttle >= 0.0f ? std::clamp(double(car.throttle), 0.0, 1.0) : 0.5;
          u.view = View::kDefault;
          own->pitch_scale = std::clamp(double(doppler), 0.5, 2.0);
          own->set(u);
          mix_other_engine(sl, *own, atten * view_other, pl, pr, other_bus, frames);
        } else {
          sl.eng_gain = 0.0f;  // still loading, or no engine sound: silent (no stand-in)
        }
        // The shared opponent loops fade out if they were playing.
        if (sl.lo_voice.gain > 1e-4f) mix_loop(sl.lo_voice, sl.last_lo_id, 0.0f, sl.lo_voice.pitch, pl, pr, other_bus, frames);
        if (sl.last_hi_id != SfxSoundId::kCount && sl.hi_voice.gain > 1e-4f)
          mix_loop(sl.hi_voice, sl.last_hi_id, 0.0f, sl.hi_voice.pitch, pl, pr, other_bus, frames);
      } else {
        sl.eng_gain = 0.0f;
        mix_shared_loops = true;
      }

      // Without the car's own sound: the shared opponent loops (no disc
      // data for the car, or the electric motor).
      SfxSoundId lo_id = SfxSoundId::kOtherCarLoop;
      SfxSoundId hi_id = SfxSoundId::kCount;
      if (mix_shared_loops) {
        if (car.is_traffic || car.archetype == 6) {
          lo_id = resolve_fallback(SfxSoundId::kTrafficCarLoop, SfxSoundId::kOtherCarLoop);
          hi_id = SfxSoundId::kTrafficCarLoopHi;
        } else if (car.archetype == 5) {
          lo_id = resolve_fallback(SfxSoundId::kElectricMotor, SfxSoundId::kOtherCarLoop);
          hi_id = SfxSoundId::kElectricMotorHigh;
        } else if (car.archetype == 4) {
          lo_id = resolve_fallback(SfxSoundId::kOtherCarLoop4, SfxSoundId::kOtherCarLoop);
          hi_id = SfxSoundId::kOtherCarLoop4Hi;
        } else {
          const uint8_t bank_idx = (car.archetype >= 1 && car.archetype <= 3) ? car.archetype : uint8_t(car.variant & 3u);
          lo_id = resolve_fallback(kOppLoBanks[bank_idx], SfxSoundId::kOtherCarLoop);
          hi_id = kOppHiBanks[bank_idx];
        }
        const bool has_hi = (hi_id != SfxSoundId::kCount && hi_id != lo_id && size_t(hi_id) < samples_.size() &&
                             samples_[size_t(hi_id)] && !samples_[size_t(hi_id)]->data.empty());
        sl.last_lo_id = lo_id;
        sl.last_hi_id = has_hi ? hi_id : SfxSoundId::kCount;

        const float base_gain = 0.55f * atten * view_other;
        if (has_hi) {
          const float xf_lo = (car.is_traffic || car.archetype == 6) ? 2200.0f : 2800.0f;
          const float xf_span = (car.is_traffic || car.archetype == 6) ? 1800.0f : 2400.0f;
          const float u = std::clamp((car.rpm - xf_lo) / xf_span, 0.0f, 1.0f);
          const float hi_w = std::sin(u * 1.5707963f);
          const float lo_w = std::cos(u * 1.5707963f);
          const float lo_pitch = std::clamp((car.rpm / 2600.0f) * p_scale, 0.40f, 2.4f) * doppler;
          const float hi_pitch = std::clamp((car.rpm / 5400.0f) * p_scale, 0.45f, 2.4f) * doppler;
          mix_loop(sl.lo_voice, lo_id, base_gain * lo_w, lo_pitch, pl, pr, other_bus, frames);
          mix_loop(sl.hi_voice, hi_id, base_gain * hi_w, hi_pitch, pl, pr, other_bus, frames);
        } else {
          const float base_pitch = std::clamp((car.rpm / 3000.0f) * p_scale, 0.40f, 2.8f) * doppler;
          mix_loop(sl.lo_voice, lo_id, base_gain, base_pitch, pl, pr, other_bus, frames);
          sl.hi_voice.gain = 0.0f;
        }
      }

      // LOD2 3D tyre roll & cornering skid layer routed by surface_group, smoothly fading out across 50..65 m.
      const float base_tire_atten = std::clamp(1.0f - dist / 65.0f, 0.0f, 1.0f);
      const float edge_fade = (dist <= 50.0f) ? 1.0f : std::clamp((65.0f - dist) / 15.0f, 0.0f, 1.0f);
      const float tire_atten = base_tire_atten * edge_fade;
      const float roll_amt = std::clamp((car.speed_mph - 12.0f) / 90.0f, 0.0f, 1.0f);
      const float slip_amt = safe_clamp_f(car.slip, 0.0f, 1.0f, 0.0f);
      const bool skidding = slip_amt > 0.35f;
      const SfxSoundId t_sid = skidding ? skid_for_surface(car.surface_group) : roll_for_surface(car.surface_group);
      sl.last_tire_id = t_sid;
      const float t_gain =
          (dist < 65.0f && car.speed_mph > 12.0f)
              ? ((skidding ? (0.30f * slip_amt) : (0.16f * roll_amt)) * tire_atten * view_other)
              : 0.0f;
      const float t_pitch = std::clamp((0.72f + 0.55f * roll_amt) * doppler, 0.45f, 2.1f);
      mix_loop(sl.tire_voice, t_sid, t_gain, t_pitch, pl, pr, other_bus, frames);

      // Near-miss Whoosh3D flyby when passing another car closely at high relative speed.
      const float rvx = car.vel.x - st.listener_vel.x;
      const float rvy = car.vel.y - st.listener_vel.y;
      const float rvz = car.vel.z - st.listener_vel.z;
      const float rel_speed = std::sqrt(rvx * rvx + rvy * rvy + rvz * rvz);
      if (dist < 10.0f && rel_speed > 20.0f && (time_s_ - sl.whoosh_cooldown_s) > 1.5) {
        sl.whoosh_cooldown_s = time_s_;
        const float wg = 0.55f * std::clamp(rel_speed / 50.0f, 0.3f, 1.0f) * std::clamp(1.0f - dist / 12.0f, 0.2f, 1.0f) *
                         view_other;
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
      const float target_gain = 0.72f * atten * view_other;

      SfxSoundId sid = SfxSoundId::kShowcaseP51;
      switch (st.showcase.kind) {
        case 1: sid = SfxSoundId::kShowcaseBalloon; break;
        case 2: sid = SfxSoundId::kShowcaseJet; break;  // Never alias Jet onto P-51 propeller audio
        case 3: sid = resolve_fallback(SfxSoundId::kShowcaseBiplane, SfxSoundId::kShowcaseP51); break;
        case 4: sid = resolve_fallback(SfxSoundId::kShowcaseHeli, SfxSoundId::kShowcaseP51); break;
        default: sid = SfxSoundId::kShowcaseP51; break;
      }
      const PcmSample* sc_pcm = samples_[size_t(sid)];
      if (sc_pcm && !sc_pcm->data.empty()) {
        mix_loop(showcase_voice_, sid, target_gain, target_pitch, pl, pr, other_bus, frames);
      } else if (procedural_mode_ && (target_gain > 1e-4f || showcase_voice_.gain > 1e-4f)) {
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

  // 5. Mix active one-shot voices into their respective buses (with 5 ms click-free steal fade-out
  // and exclusive mono `lfe_out` routing for ImpactLFE & BackfireLFE when `lfe_out != nullptr`).
  auto resolve_shot_bus = [&](Bus b) -> float* {
    float* bus = buses[int(b)];
    if (!bus) {
      if (b == Bus::kCollision) bus = buses[int(Bus::kAmbience)];
      else if (b == Bus::kTransmission || b == Bus::kSupercharger || b == Bus::kTurbo ||
               b == Bus::kEngineDamage || b == Bus::kBackfire || b == Bus::kSuspension)
        bus = buses[int(Bus::kEngine)];
    }
    return bus;
  };
  for (auto& o : one_shots_) {
    if (!o.active) continue;
    const PcmSample* pcm = nullptr;
    float* bus = nullptr;
    size_t n = 0;
    float o_gain = 0.0f, o_pitch = 1.0f, o_left = 0.707f, o_right = 0.707f;
    double step = 1.0, attack_n = 0.0, release_n = 0.0;
    bool route_mono_lfe = false;

    auto bind_current = [&]() -> bool {
      const size_t idx = size_t(o.id);
      if (o.ui >= 0) pcm = o.ui < kMaxUiSamples ? ui_samples_[size_t(o.ui)] : nullptr;
      else pcm = idx < samples_.size() ? samples_[idx] : nullptr;
      if (!pcm || pcm->data.empty() || !std::isfinite(o.phase) || o.phase < 0.0) return false;
      bus = resolve_shot_bus(o.bus);
      n = pcm->data.size();
      o_gain = safe_clamp_f(o.gain, 0.0f, 4.0f, 0.0f);
      o_pitch = safe_clamp_f(o.pitch, 0.25f, 4.0f, 1.0f);
      o_left = safe_clamp_f(o.left, 0.0f, 2.0f, 0.707f);
      o_right = safe_clamp_f(o.right, 0.0f, 2.0f, 0.707f);
      step = double(o_pitch) * double(std::max(1, pcm->rate)) / double(rate_);
      attack_n = std::min<double>(64.0, double(n) * 0.125);
      release_n = std::min<double>(64.0, double(n) * 0.125);
      route_mono_lfe = (lfe_out != nullptr) && o.ui < 0 &&
                       (o.id == SfxSoundId::kImpactLfe || o.id == SfxSoundId::kBackfireLfe);
      return true;
    };
    auto promote_steal = [&]() -> bool {
      o.id = o.next_id;
      o.ui = o.next_ui;
      o.bus = o.next_bus;
      o.phase = 0.0;
      o.gain = o.next_gain;
      o.left = o.next_left;
      o.right = o.next_right;
      o.pitch = o.next_pitch;
      o.priority = o.next_priority;
      o.spawn_seq = o.next_spawn_seq;
      o.stealing = false;
      o.fade_rem_frames = 0;
      o.fade_total_frames = 0;
      return bind_current();
    };

    if (!bind_current()) {
      if (o.stealing && promote_steal()) {
        // Promoted directly to queued replacement voice.
      } else {
        o.active = false;
        continue;
      }
    }

    for (int f = 0; f < frames; ++f) {
      if (o.stealing && o.fade_rem_frames <= 0) {
        if (!promote_steal()) {
          o.active = false;
          break;
        }
      }
      const size_t k = size_t(o.phase);
      if (k >= n) {
        if (o.stealing) {
          if (!promote_steal()) {
            o.active = false;
            break;
          }
          continue;
        }
        o.active = false;
        break;
      }
      float steal_env = 1.0f;
      if (o.stealing) {
        steal_env = float(std::max(0, o.fade_rem_frames)) / float(std::max(1, o.fade_total_frames));
        --o.fade_rem_frames;
      }
      if (route_mono_lfe || bus) {
        const double frac = o.phase - double(k);
        const float a = pcm->data[k];
        const float b = (k + 1 < n) ? pcm->data[k + 1] : 0.0f;
        float env = steal_env;
        if (attack_n > 1.0 && o.phase < attack_n) env *= float(o.phase / attack_n);
        const double rem = double(n) - o.phase;
        if (release_n > 1.0 && rem < release_n) env = std::min(env, float(std::max(0.0, rem / release_n)) * steal_env);
        const float s = (a + (b - a) * float(frac)) * o_gain * env;
        if (std::isfinite(s)) {
          if (route_mono_lfe) {
            lfe_out[f] += s;
          } else {
            bus[2 * f] += s * o_left;
            bus[2 * f + 1] += s * o_right;
          }
        }
      }
      o.phase += step;
    }
  }
}

}  // namespace audio
