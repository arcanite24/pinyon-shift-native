#include "audio/satnav_vo.h"

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
#include "audio/xma_decode.h"
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

bool decode_fsb_clip(const std::filesystem::path& bank_path, uint32_t subsound, const std::filesystem::path& ffmpeg,
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
    const auto in_p = temp_dir / ("satnav_" + std::to_string(subsound) + ".wav");
    const auto out_p = temp_dir / ("satnav_" + std::to_string(subsound) + ".s16");
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

bool resolve_fev_suffix(const FevProject& proj, std::string_view suffix, std::filesystem::path& bank_out,
                        uint32_t& subsound_out) {
  const std::string want = lower_str(std::string(suffix));
  for (const auto& ev : proj.events) {
    const std::string lp = lower_str(ev.path);
    if (lp.size() < want.size() || lp.compare(lp.size() - want.size(), want.size(), want) != 0) continue;
    auto pick = [&](const std::vector<FevSoundInstance>& insts) -> bool {
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
    if (pick(ev.simple_instances)) return true;
    for (const auto& ly : ev.layers)
      if (pick(ly.instances)) return true;
  }
  return false;
}

struct CueSpec {
  SatNavCue cue;
  const char* metric_event;
  const char* imperial_event;
  const char* fsb_needle;
};

constexpr CueSpec kSatNavCueSpecs[] = {
    {SatNavCue::kDestGeneric, "/SetDestination/DestinationSet_Generic", "/SetDestination/DestinationSet_Generic", "DestinationSet_Default"},
    {SatNavCue::kDestFestival, "/SetDestination/DestinationSet_Festival", "/SetDestination/DestinationSet_Festival", "DestinationSet_Festival"},
    {SatNavCue::kDestFestivalRace, "/SetDestination/DestinationSet_FestivalRace", "/SetDestination/DestinationSet_FestivalRace", "DestinationSet_FestivalRace"},
    {SatNavCue::kDestShowcase, "/SetDestination/DestinationSet_Showcase", "/SetDestination/DestinationSet_Showcase", "DestinationSet_Showcase"},
    {SatNavCue::kDestOutpost, "/SetDestination/DestinationSet_HorizonOutpost", "/SetDestination/DestinationSet_HorizonOutpost", "DestinationSet_HorizonOutpost"},
    {SatNavCue::kDestWorkshop, "/SetDestination/DestinationSet_Workshop", "/SetDestination/DestinationSet_Workshop", "DestinationSet_Workshop"},
    {SatNavCue::kDestAutoshow, "/SetDestination/DestinationSet_Autoshow", "/SetDestination/DestinationSet_Autoshow", "DestinationSet_Autoshow"},
    {SatNavCue::kDestPaintShop, "/SetDestination/DestinationSet_Paintshop", "/SetDestination/DestinationSet_Paintshop", "DestinationSet_PaintShop"},
    {SatNavCue::kDestRaceCentral, "/SetDestination/DestinationSet_RaceCentral", "/SetDestination/DestinationSet_RaceCentral", "DestinationSet_RaceCentral"},
    {SatNavCue::kDestStreetRace, "/SetDestination/DestinationSet_StreetRace", "/SetDestination/DestinationSet_StreetRace", "DestinationSet_StreetRace"},
    {SatNavCue::kDestReset, "/SetDestination/DestinationReset", "/SetDestination/DestinationReset", "DestinationReset"},
    {SatNavCue::kDestCannotBeSet, "/SetDestination/DestinationCannotBeSet", "/SetDestination/DestinationCannotBeSet", "DestinationCannotBeSet"},
    {SatNavCue::kRecalculating, "/Recalculating", "/Recalculating", "Recalculating"},
    {SatNavCue::kUTurn, "/TurnAround", "/TurnAround", "UTurn"},
    {SatNavCue::kTurnLeft, "/Left", "/Left", "left_Turn"},
    {SatNavCue::kTurnRight, "/Right", "/Right", "Right_Turn"},
    {SatNavCue::kBearLeft, "/BearLeft", "/BearLeft", "left_Bear"},
    {SatNavCue::kBearRight, "/BearRight", "/BearRight", "Right_Bear"},
    {SatNavCue::kSharpLeft, "/SharpLeft", "/SharpLeft", "left_Sharp"},
    {SatNavCue::kSharpRight, "/SharpRight", "/SharpRight", "Right_Sharp"},
    {SatNavCue::kNearLeft, "/Metric/NearLeftMetric", "/Imperial/NearLeft", "Metric_left_NearTurn"},
    {SatNavCue::kNearRight, "/Metric/NearRightMetric", "/Imperial/NearRight", "Metric_Right_NearTurn"},
    {SatNavCue::kNearBearLeft, "/Metric/NearBearLeftMetric", "/Imperial/NearBearLeft", "Metric_left_NearBear"},
    {SatNavCue::kNearBearRight, "/Metric/NearBearRightMetric", "/Imperial/NearBearRight", "Metric_Right_NearBear"},
    {SatNavCue::kNearSharpLeft, "/Metric/NearSharpLeftMetric", "/Imperial/NearSharpLeft", "Metric_left_NearSharp"},
    {SatNavCue::kNearSharpRight, "/Metric/NearSharpRightMetric", "/Imperial/NearSharpRight", "Metric_Right_NearSharp"},
    {SatNavCue::kFarLeft, "/Metric/FarLeftMetric", "/Imperial/FarLeft", "Metric_left_FarTurn"},
    {SatNavCue::kFarRight, "/Metric/FarRightMetric", "/Imperial/FarRight", "Metric_Right_FarTurn"},
    {SatNavCue::kFarBearLeft, "/Metric/FarBearLeftMetric", "/Imperial/FarBearLeft", "Metric_left_FarBear"},
    {SatNavCue::kFarBearRight, "/Metric/FarBearRightMetric", "/Imperial/FarBearRight", "Metric_Right_FarBear"},
    {SatNavCue::kFarSharpLeft, "/Metric/FarSharpleftMetric", "/Imperial/FarSharpLeft", "Metric_left_FarSharp"},
    {SatNavCue::kFarSharpRight, "/Metric/FarSharpRightMetric", "/Imperial/FarSharpRight", "Metric_Right_FarSharp"},
    {SatNavCue::kExtendedLeft, "/Metric/SatNav_Metric_Left_ExtendedTurn", "/Imperial/SatNav_Imperial_Left_ExtendedTurn", "Metric_Left_ExtendedTurn"},
    {SatNavCue::kExtendedRight, "/Metric/SatNav_Metric_Right_ExtendedTurn", "/Imperial/SatNav_Imperial_Right_ExtendedTurn", "Metric_Right_ExtendedTurn"},
    {SatNavCue::kExtendedBearLeft, "/Metric/SatNav_Metric_Left_ExtendedBear", "/Imperial/SatNav_Imperial_Left_ExtendedBear", "Metric_Left_ExtendedBear"},
    {SatNavCue::kExtendedBearRight, "/Metric/SatNav_Metric_Right_ExtendedBear", "/Imperial/SatNav_Imperial_Right_ExtendedBear", "Metric_Right_ExtendedBear"},
    {SatNavCue::kExtendedSharpLeft, "/Metric/SatNav_Metric_Left_ExtendedSharp", "/Imperial/SatNav_Imperial_Left_ExtendedSharp", "Metric_Left_ExtendedSharp"},
    {SatNavCue::kExtendedSharpRight, "/Metric/SatNav_Metric_Right_ExtendedSharp", "/Imperial/SatNav_Imperial_Right_ExtendedSharp", "Metric_Right_ExtendedSharp"},
    {SatNavCue::kNextJuncLeft, "/NextJuncLeft", "/NextJuncLeft", "left_NextJuncTurn"},
    {SatNavCue::kNextJuncRight, "/NextJuncRight", "/NextJuncRight", "Right_NextJuncTurn"},
    {SatNavCue::kNextJuncBearLeft, "/NextJuncBearLeft", "/NextJuncBearLeft", "left_NextJuncBear"},
    {SatNavCue::kNextJuncBearRight, "/NextJuncBearRight", "/NextJuncBearRight", "Right_NextJuncBear"},
    {SatNavCue::kNextJuncSharpLeft, "/NextJuncSharpLeft", "/NextJuncSharpLeft", "left_NextJuncSharp"},
    {SatNavCue::kNextJuncSharpRight, "/NextJuncSharpRight", "/NextJuncSharpRight", "Right_NextJuncSharp"},
    {SatNavCue::kNearDestination, "/Metric/NearDestination", "/Imperial/NearDestination", "Metric_Destination_Near"},
    {SatNavCue::kFarDestination, "/Metric/FarDestination", "/Imperial/FarDestination", "Metric_Destination_Far"},
    {SatNavCue::kExtendedDestination, "/Metric/SatNav_Metric_Destination_ExtendedDefault", "/Imperial/SatNav_Imperial_Destination_ExtendedDefault", "Metric_Destination_Extended"},
    {SatNavCue::kArrived, "/Destinations/Destination", "/Destinations/Destination", "Destination_Default"},
    {SatNavCue::kRoundaboutExit1, "/Roundabout/Exit1", "/Roundabout/Exit1", "Roundabout_Exit1"},
    {SatNavCue::kRoundaboutExit2, "/Roundabout/Exit2", "/Roundabout/Exit2", "Roundabout_Exit2"},
    {SatNavCue::kRoundaboutExit3, "/Roundabout/Exit3", "/Roundabout/Exit3", "Roundabout_Exit3"},
    {SatNavCue::kRoundaboutExit4, "/Roundabout/Exit4", "/Roundabout/Exit4", "Roundabout_Exit4"},
    {SatNavCue::kKeepLeft, "/KeepLeft", "/KeepLeft", "Keep_Left"},
    {SatNavCue::kKeepRight, "/KeepRight", "/KeepRight", "Keep_Right"},
};

constexpr float kMonoPanGain = 0.70710678f;  // -3 dB equal-power center pan for mono SatNav VO

}  // namespace

const char* satnav_cue_fsb_fallback(SatNavCue cue) {
  for (const auto& sp : kSatNavCueSpecs) {
    if (sp.cue == cue) return sp.fsb_needle;
  }
  return "";
}

SatNavVoPlayer::SatNavVoPlayer(int out_rate) : rate_(std::max(8000, out_rate)) { clips_.fill(nullptr); }

SatNavVoPlayer::~SatNavVoPlayer() {
  stop_worker();
  drain_loaded();
  for (PcmSample*& p : clips_) {
    delete p;
    p = nullptr;
  }
}

void SatNavVoPlayer::stop_worker() {
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
}

void SatNavVoPlayer::drain_loaded() {
  LoadedClip item;
  while (loaded_q_.pop(item)) {
    const int idx = int(item.cue);
    if (idx >= 0 && idx < int(SatNavCue::kCount)) {
      if (!clips_[size_t(idx)]) clips_[size_t(idx)] = item.pcm;
      else delete item.pcm;
    } else {
      delete item.pcm;
    }
  }
}

void SatNavVoPlayer::set_clip(SatNavCue cue, PcmSample sample) {
  const int idx = int(cue);
  if (idx < 0 || idx >= int(SatNavCue::kCount)) return;
  delete clips_[size_t(idx)];
  clips_[size_t(idx)] = new PcmSample(std::move(sample));
}

bool SatNavVoPlayer::load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                          std::string_view language, bool metric, std::string* note) {
  stop_worker();
  if (game_root.empty()) return false;
  const auto vo_dir = game_root / "media" / "audio" / "vo";

  std::string lang(language.empty() ? "EN" : language);
  for (char& c : lang) c = char(std::toupper(static_cast<unsigned char>(c)));
  std::filesystem::path fev_path = cars::mods::resolve(vo_dir / ("SatNav_" + lang + ".fev"));
  std::error_code ec;
  if (!std::filesystem::exists(fev_path, ec)) {
    lang = "EN";
    fev_path = cars::mods::resolve(vo_dir / "SatNav_EN.fev");
  }
  if (!std::filesystem::exists(fev_path, ec)) {
    if (note) *note = "satnav vo: SatNav_EN.fev not found";
    return false;
  }

  FevProject proj;
  std::string err;
  if (!load_fev(fev_path, proj, &err)) {
    if (note) *note = "satnav vo: " + err;
    return false;
  }

  // Also allow fallback by FSB subsound filename substring if a FEV path differs.
  const std::filesystem::path default_fsb = cars::mods::resolve(vo_dir / ("SatNav_" + lang + ".fsb"));
  std::vector<FsbSample> fsb_samples;
  {
    std::vector<uint8_t> bytes;
    if (read_bytes(default_fsb, bytes)) read_fsb4(bytes, fsb_samples, nullptr);
  }

  std::vector<DecodeJob> jobs;
  for (const auto& sp : kSatNavCueSpecs) {
    std::filesystem::path bank;
    uint32_t sub = 0;
    const char* ev_suffix = metric ? sp.metric_event : sp.imperial_event;
    bool ok = resolve_fev_suffix(proj, ev_suffix, bank, sub);
    if (!ok && metric) ok = resolve_fev_suffix(proj, sp.imperial_event, bank, sub);
    // Try alternate roundabout / lane-keep FEV event names when banks use flat naming.
    if (!ok) {
      switch (sp.cue) {
        case SatNavCue::kRoundaboutExit1:
          ok = resolve_fev_suffix(proj, "/RoundaboutExit1", bank, sub) ||
               resolve_fev_suffix(proj, "/Roundabout_Exit1", bank, sub);
          break;
        case SatNavCue::kRoundaboutExit2:
          ok = resolve_fev_suffix(proj, "/RoundaboutExit2", bank, sub) ||
               resolve_fev_suffix(proj, "/Roundabout_Exit2", bank, sub);
          break;
        case SatNavCue::kRoundaboutExit3:
          ok = resolve_fev_suffix(proj, "/RoundaboutExit3", bank, sub) ||
               resolve_fev_suffix(proj, "/Roundabout_Exit3", bank, sub);
          break;
        case SatNavCue::kRoundaboutExit4:
          ok = resolve_fev_suffix(proj, "/RoundaboutExit4", bank, sub) ||
               resolve_fev_suffix(proj, "/Roundabout_Exit4", bank, sub);
          break;
        case SatNavCue::kKeepLeft:
          ok = resolve_fev_suffix(proj, "/Keep_Left", bank, sub);
          break;
        case SatNavCue::kKeepRight:
          ok = resolve_fev_suffix(proj, "/Keep_Right", bank, sub);
          break;
        default: break;
      }
    }
    if (!ok && !fsb_samples.empty() && sp.fsb_needle) {
      auto find_needle = [&](std::string_view n) -> bool {
        if (n.empty()) return false;
        const std::string needle = lower_str(std::string(n));
        // Prefer delimiter-bounded matches so e.g. DestinationSet_Festival does not
        // collide with DestinationSet_FestivalRace.
        for (int pass = 0; pass < 2; ++pass) {
          for (uint32_t i = 0; i < fsb_samples.size(); ++i) {
            const std::string nm = lower_str(fsb_samples[i].name);
            const size_t pos = nm.find(needle);
            if (pos == std::string::npos) continue;
            if (pass == 0) {
              const size_t end = pos + needle.size();
              if (end < nm.size() && std::isalpha(static_cast<unsigned char>(nm[end]))) continue;
            }
            bank = default_fsb;
            sub = i;
            return true;
          }
        }
        return false;
      };
      ok = find_needle(sp.fsb_needle);
      if (!ok && sp.cue == SatNavCue::kNearBearRight) {
        ok = find_needle("BearRight_Near") || find_needle("Right_NearBear");
      }
      if (!ok && std::string_view(sp.fsb_needle).rfind("Metric_", 0) == 0) {
        ok = find_needle(std::string_view(sp.fsb_needle).substr(7));
      }
    }
    // Graceful directional FEV fallback when a stock disc bank lacks dedicated
    // roundabout-exit or lane-keep waveforms.
    if (!ok) {
      switch (sp.cue) {
        case SatNavCue::kRoundaboutExit1: ok = resolve_fev_suffix(proj, "/Right", bank, sub); break;
        case SatNavCue::kRoundaboutExit2: ok = resolve_fev_suffix(proj, "/BearRight", bank, sub); break;
        case SatNavCue::kRoundaboutExit3: ok = resolve_fev_suffix(proj, "/Left", bank, sub); break;
        case SatNavCue::kRoundaboutExit4: ok = resolve_fev_suffix(proj, "/TurnAround", bank, sub); break;
        case SatNavCue::kKeepLeft: ok = resolve_fev_suffix(proj, "/BearLeft", bank, sub); break;
        case SatNavCue::kKeepRight: ok = resolve_fev_suffix(proj, "/BearRight", bank, sub); break;
        default: break;
      }
    }
    if (ok && std::filesystem::exists(bank, ec)) jobs.push_back({sp.cue, bank, sub});
  }

  if (jobs.empty()) {
    if (note) *note = "satnav vo: no playable clips resolved from " + fev_path.filename().string();
    return false;
  }

  if (note) {
    *note = "satnav vo: " + std::to_string(jobs.size()) + " cues queued from " + fev_path.filename().string();
  }
  start_worker(std::move(jobs), cache_dir);
  return true;
}

void SatNavVoPlayer::start_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir) {
  stop_worker_.store(false, std::memory_order_release);
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir = std::move(cache_dir)]() {
    const std::filesystem::path ffmpeg = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
    std::error_code ec;
    const std::filesystem::path cdir = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "satnav");
    const std::filesystem::path tmp = cdir.empty() ? std::filesystem::temp_directory_path(ec) : cdir;
    if (!tmp.empty()) std::filesystem::create_directories(tmp, ec);

    for (const auto& job : jobs) {
      if (stop_worker_.load(std::memory_order_acquire)) break;
      const std::string key = job.bank_path.stem().string() + "_" + std::to_string(job.subsound) + ".sfx";
      const std::filesystem::path cpath = cdir.empty() ? std::filesystem::path{} : (cdir / key);

      auto pcm = std::make_unique<PcmSample>();
      bool ok = !cpath.empty() && read_cached_pcm(cpath, *pcm);
      if (!ok) {
        ok = decode_fsb_clip(job.bank_path, job.subsound, ffmpeg, tmp, *pcm);
        if (ok && !cpath.empty()) write_cached_pcm(cpath, *pcm);
      }
      if (!ok) {
        // Android / no-ffmpeg fallback: gentle two-tone SatNav confirmation chime so turn cues stay audible.
        constexpr int kRate = 24000;
        const size_t n = size_t(0.24 * kRate);
        pcm->rate = kRate;
        pcm->data.resize(n);
        const double f0 = (int(job.cue) & 1) ? 740.0 : 620.0;
        const double f1 = f0 * 1.25;
        double ph = 0.0;
        for (size_t i = 0; i < n; ++i) {
          const double u = double(i) / double(n);
          const double f = u < 0.48 ? f0 : f1;
          ph += 6.283185307179586 * f / double(kRate);
          const double env = std::sin(3.14159265 * std::fmod(u * 2.0, 1.0)) * std::exp(-2.2 * u);
          pcm->data[i] = float(0.22 * env * std::sin(ph));
        }
        ok = true;
      }
      if (ok && !pcm->data.empty()) {
        if (!loaded_q_.push({job.cue, pcm.get()})) continue;
        pcm.release();
      }
    }
  });
}

void SatNavVoPlayer::set_enabled(bool on) {
  enabled_ = on;
  if (!enabled_) stop();
}

bool SatNavVoPlayer::start_next_pending() {
  while (pending_head_ != pending_tail_) {
    const SatNavCue next = pending_[pending_head_];
    pending_head_ = (pending_head_ + 1) % kMaxPending;
    const int idx = int(next);
    if (idx >= 0 && idx < int(SatNavCue::kCount) && clips_[size_t(idx)] && !clips_[size_t(idx)]->data.empty()) {
      active_ = true;
      active_cue_ = next;
      phase_ = 0.0;
      return true;
    }
  }
  active_ = false;
  phase_ = 0.0;
  return false;
}

void SatNavVoPlayer::trigger(SatNavCue cue) {
  drain_loaded();
  if (!enabled_) return;
  const int idx = int(cue);
  if (idx < 0 || idx >= int(SatNavCue::kCount)) return;
  if (!clips_[size_t(idx)] || clips_[size_t(idx)]->data.empty()) return;

  // High-priority interrupts clear stale queued turn calls and start cleanly.
  const bool interrupt = (cue == SatNavCue::kRecalculating || cue == SatNavCue::kArrived ||
                          cue == SatNavCue::kDestReset || cue == SatNavCue::kDestCannotBeSet ||
                          cue == SatNavCue::kUTurn);
  if (interrupt || !active_) {
    if (interrupt) pending_head_ = pending_tail_ = 0;
    if (interrupt && active_ && phase_ > 0.0) {
      // Smooth 8 ms fade-out of the interrupted SatNav clip to prevent clicks.
      fade_cue_ = active_cue_;
      fade_phase_ = phase_;
      fade_total_ = std::max(1, (rate_ * 8) / 1000);
      fade_rem_ = fade_total_;
      fade_last_ = last_sample_;
    }
    active_ = true;
    active_cue_ = cue;
    phase_ = 0.0;
    return;
  }

  // Avoid queuing duplicate back-to-back cues.
  if (pending_head_ == pending_tail_ && active_cue_ == cue) return;
  if (pending_head_ != pending_tail_) {
    const size_t last = (pending_tail_ + kMaxPending - 1) % kMaxPending;
    if (pending_[last] == cue) return;
  }
  const size_t next_tail = (pending_tail_ + 1) % kMaxPending;
  if (next_tail == pending_head_) {
    // Drop oldest queued turn cue so recent navigation prompts win.
    pending_head_ = (pending_head_ + 1) % kMaxPending;
  }
  pending_[pending_tail_] = cue;
  pending_tail_ = next_tail;
}

void SatNavVoPlayer::stop() {
  pending_head_ = pending_tail_ = 0;
  active_ = false;
  phase_ = 0.0;
  last_sample_ = 0.0f;
  fade_rem_ = 0;
  fade_total_ = 0;
  fade_phase_ = 0.0;
  fade_last_ = 0.0f;
}

bool SatNavVoPlayer::render(float* vo_bus, int frames) {
  drain_loaded();
  if (!enabled_ || !vo_bus || frames <= 0) {
    active_ = false;
    fade_rem_ = 0;
    return false;
  }
  if (!active_ && fade_rem_ <= 0 && !start_next_pending()) return false;

  int i = 0;
  while (i < frames && (active_ || fade_rem_ > 0)) {
    const PcmSample* s = active_ ? clips_[size_t(active_cue_)] : nullptr;
    if (active_ && (!s || s->data.empty() || s->rate <= 0)) {
      if (!start_next_pending() && fade_rem_ <= 0) break;
      continue;
    }
    const double step = (s && s->rate > 0) ? (double(s->rate) / double(rate_)) : 0.0;
    const size_t n = s ? s->data.size() : 0;
    const PcmSample* fs = (fade_rem_ > 0) ? clips_[size_t(fade_cue_)] : nullptr;
    const double fstep = (fs && fs->rate > 0) ? (double(fs->rate) / double(rate_)) : 0.0;
    const size_t fn = fs ? fs->data.size() : 0;

    while (i < frames) {
      float out_v = 0.0f;
      float in_gain = 1.0f;
      if (fade_rem_ > 0 && fade_total_ > 0) {
        const float fade_g = float(fade_rem_ - 1) / float(fade_total_);
        float fv = fade_last_;
        if (fs && fn >= 2 && fade_phase_ < double(fn - 1)) {
          const size_t fp0 = size_t(fade_phase_);
          const float ffrac = float(fade_phase_ - double(fp0));
          fv = (fs->data[fp0] * (1.0f - ffrac) + fs->data[fp0 + 1] * ffrac) * gain_ * kMonoPanGain;
          fade_last_ = fv;
          fade_phase_ += fstep;
        }
        out_v += fv * fade_g;
        in_gain = 1.0f - fade_g;
        --fade_rem_;
      }

      if (active_ && s) {
        if (n < 2 || phase_ >= double(n - 1)) {
          start_next_pending();
          if (out_v != 0.0f) {
            vo_bus[2 * i] += out_v;
            vo_bus[2 * i + 1] += out_v;
            ++i;
          }
          break;
        }
        const size_t p0 = size_t(phase_);
        const float frac = float(phase_ - double(p0));
        const float v = (s->data[p0] * (1.0f - frac) + s->data[p0 + 1] * frac) * gain_ * kMonoPanGain;
        const float mixed = out_v + v * in_gain;
        last_sample_ = mixed;
        vo_bus[2 * i] += mixed;
        vo_bus[2 * i + 1] += mixed;
        phase_ += step;
        ++i;
      } else {
        last_sample_ = out_v;
        vo_bus[2 * i] += out_v;
        vo_bus[2 * i + 1] += out_v;
        ++i;
        if (fade_rem_ <= 0) break;
      }
    }
  }
  return active_ || fade_rem_ > 0 || i > 0;
}

int SatNavVoPlayer::loaded_clip_count() const {
  int n = 0;
  for (const PcmSample* p : clips_)
    if (p && !p->data.empty()) ++n;
  return n;
}

}  // namespace audio
