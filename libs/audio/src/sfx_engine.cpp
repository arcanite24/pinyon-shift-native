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
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
}

void SfxEngine::drain_loaded() {
  LoadedSample ls;
  while (loaded_q_.pop(ls)) {
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

void SfxEngine::set_sample(SfxSoundId id, PcmSample sample) {
  const size_t idx = size_t(id);
  if (idx >= samples_.size()) return;
  auto* p = new PcmSample(std::move(sample));
  if (!loaded_q_.push({id, p})) delete p;
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
                 {SfxSoundId::kSurfaceHardToHard, "Tires/Tires/SurfaceChanges/HardToHard"},
                 {SfxSoundId::kSurfaceDirtToHard, "Tires/Tires/SurfaceChanges/DirtToHard"}});
  add_batch_fev("cars/Transmissions/Transmissions.fev",
                {{SfxSoundId::kTransWhine, "Transmissions/Transmissions/L0"},
                 {SfxSoundId::kShiftStart, "Transmissions/Transmissions/ShiftStart/L0"},
                 {SfxSoundId::kShiftComplete, "Transmissions/Transmissions/ShiftComplete/L0"}});
  add_batch_fev("cars/Turbos/Turbos.fev",
                {{SfxSoundId::kTurboSpool, "Turbos/Turbos/L1 Group/L1"},
                 {SfxSoundId::kTurboBovLo, "Turbos/Turbos/L1 Group/L1_BlowOff_1_Lo"},
                 {SfxSoundId::kTurboBovHi, "Turbos/Turbos/L1 Group/L1_BlowOff_1_Hi"}});
  add_from_fev("cars/SuperChargers/SuperChargers.fev", SfxSoundId::kScWhine,
               "SuperChargers/SuperChargers/Roots_TwinScrew/L1");
  add_from_fev("cars/Suspension/Suspension.fev", SfxSoundId::kSuspensionBottom, "Suspension/Suspension/BottomOut");
  add_from_fev("cars/EngineLFE/EngineLFE.fev", SfxSoundId::kBackfireLfe, "EngineLFE/Backfire/IMPACT_LFE");

  // Collisions via CollisionData.xml -> Collisions.fev.
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
      // First BreakEvent for sign/prop smashes.
      for (const auto& p : coll_cfg_.pairs) {
        if (p.is_break && !p.impact3d.empty() && resolve_event_sample(coll_fev, p.impact3d, bank, sub)) {
          jobs.push_back({SfxSoundId::kBreakProp, bank, sub});
          break;
        }
      }
    }
  }

  // UI, Rewind TapeStart, and Race Countdown via ui4audio.xml -> UIInGame.fev.
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
      add_ui(SfxSoundId::kSignSmash, "Hud_Collection", "UIInGame/UI/HorizonHud/COLLECTION");
      add_ui(SfxSoundId::kAchievementReward, "HUD_General_Notification_Reward",
             "UIInGame/UI/HorizonHud/HUD_General_Notification_Reward");
      // Race 3-2-1 countdown cue.
      for (const auto& ev : ui_fev.events) {
        if (lower_str(ev.path).find("horizoncountdowns/") != std::string::npos) {
          std::filesystem::path bank;
          uint32_t sub = 0;
          if (resolve_event_sample(ui_fev, ev.path, bank, sub)) {
            jobs.push_back({SfxSoundId::kCountdown321, bank, sub});
            break;
          }
        }
      }
    }
  }

  // Exhaust burble / backfire bank (Burbles/*.fsb).
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
    if (!bpath.empty()) jobs.push_back({SfxSoundId::kBackfirePop, bpath, 0});
  }

  // Representative 3D opponent and traffic engine loops from LOD1 banks.
  {
    const auto lod1 = audio_dir / "cars" / "Engines" / "Soundbanks" / "LOD1";
    std::error_code ec;
    std::filesystem::path opp_bank, traffic_bank;
    if (std::filesystem::is_directory(lod1, ec)) {
      for (const auto& e : std::filesystem::directory_iterator(lod1, ec)) {
        if (!e.is_regular_file(ec) || lower_str(e.path().extension().string()) != ".fsb") continue;
        const std::string fn = lower_str(e.path().filename().string());
        if (traffic_bank.empty() && fn.find("traffic") != std::string::npos) traffic_bank = e.path();
        else if (opp_bank.empty() && fn.find("_exh.fsb") != std::string::npos) opp_bank = e.path();
        if (!opp_bank.empty() && !traffic_bank.empty()) break;
      }
    }
    if (!opp_bank.empty()) jobs.push_back({SfxSoundId::kOtherCarLoop, opp_bank, 4});
    if (!traffic_bank.empty()) jobs.push_back({SfxSoundId::kTrafficCarLoop, traffic_bank, 2});
    else if (!opp_bank.empty()) jobs.push_back({SfxSoundId::kTrafficCarLoop, opp_bank, 2});
  }

  if (note) *note = "sfx engine: queued " + std::to_string(jobs.size()) + " FEV/FSB4 sounds for async decode";
  if (!jobs.empty()) start_decoder_worker(std::move(jobs), cache_dir);
  return true;
}

void SfxEngine::start_decoder_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir) {
  stop_worker_.store(false, std::memory_order_release);
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir = std::move(cache_dir)]() {
    const std::filesystem::path ffmpeg = find_ffmpeg();
    std::error_code ec;
    const std::filesystem::path sfx_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "sfx");
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(ec) /
        ("shift-sfx-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, ec);
    for (const auto& job : jobs) {
      if (stop_worker_.load(std::memory_order_acquire)) break;
      const std::filesystem::path cache_file =
          sfx_cache.empty()
              ? std::filesystem::path{}
              : (sfx_cache / (lower_str(job.bank_path.stem().string()) + "." + std::to_string(job.subsound) + ".s16"));
      auto pcm = std::make_unique<PcmSample>();
      bool ok = !cache_file.empty() && read_cached_sfx(cache_file, *pcm);
      if (!ok) {
        ok = decode_fsb_subsound(job.bank_path, job.subsound, ffmpeg, temp_dir, *pcm);
        if (ok && !cache_file.empty()) write_cached_sfx(cache_file, *pcm);
      }
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
    case UiCue::kSkillAward: id = SfxSoundId::kUiAccept; gain = 0.65f; break;
    case UiCue::kAchievementReward:
      id = samples_[size_t(SfxSoundId::kAchievementReward)] ? SfxSoundId::kAchievementReward : SfxSoundId::kSignSmash;
      gain = 0.90f;
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

  // 2. Tyres roll/skid & surface changes (Bus::kTyres)
  {
    float max_slip = 0.0f, avg_load = 0.0f;
    int offroad_count = 0, grass_count = 0;
    for (int w = 0; w < 4; ++w) {
      const auto& wh = st.wheels[size_t(w)];
      max_slip = std::max(max_slip, wh.slip_combined);
      avg_load += std::clamp(wh.load, 0.0f, 2.0f);
      if (wh.surface_group == 1) ++offroad_count;
      else if (wh.surface_group == 2) ++grass_count;
      if (wh.surface_group != prev_wheel_surface_[size_t(w)] && st.speed_mph > 5.0f) {
        const bool dirt_edge = (wh.surface_group == 1 || wh.surface_group == 2 ||
                                prev_wheel_surface_[size_t(w)] == 1 || prev_wheel_surface_[size_t(w)] == 2);
        const float g = 0.35f * std::clamp(st.speed_mph / 106.0f, 0.1f, 1.0f);
        spawn_one_shot(dirt_edge ? SfxSoundId::kSurfaceDirtToHard : SfxSoundId::kSurfaceHardToHard, Bus::kTyres, g,
                       0.707f, 0.707f);
      }
      prev_wheel_surface_[size_t(w)] = wh.surface_group;

      // Suspension BottomOut (Bus::kEngine)
      if (wh.bottom_out_force > 0.002f && (time_s_ - susp_cooldown_s_[size_t(w)]) > 0.25) {
        susp_cooldown_s_[size_t(w)] = time_s_;
        const float sg = 0.6f * std::clamp(wh.bottom_out_force / 0.07f, 0.2f, 1.0f);
        spawn_one_shot(SfxSoundId::kSuspensionBottom, Bus::kEngine, sg, (w & 1) ? 0.55f : 0.85f,
                       (w & 1) ? 0.85f : 0.55f);
      }
    }
    avg_load *= 0.25f;
    const SfxSoundId roll_id = grass_count >= 2    ? SfxSoundId::kTireRollGrass
                               : offroad_count >= 2 ? SfxSoundId::kTireRollOffRoad
                                                    : SfxSoundId::kTireRollOnRoad;
    const SfxSoundId skid_id = grass_count >= 2    ? SfxSoundId::kTireSkidGrass
                               : offroad_count >= 2 ? SfxSoundId::kTireSkidOffRoad
                                                    : SfxSoundId::kTireSkidOnRoad;
    if (float* tyre_bus = buses[int(Bus::kTyres)]) {
      const float roll_g = 0.35f * std::clamp(st.speed_mph / 90.0f, 0.0f, 1.0f) * std::clamp(avg_load, 0.0f, 1.2f);
      const float roll_p = 0.65f + 0.75f * std::clamp(st.speed_mph / 130.0f, 0.0f, 1.0f);
      mix_loop(tire_roll_voice_, roll_id, roll_g, roll_p, 0.707f, 0.707f, tyre_bus, frames);

      const float skid_g =
          max_slip > 0.15f && st.speed_mph > 4.0f ? 0.65f * std::clamp((max_slip - 0.15f) / 0.85f, 0.0f, 1.0f) : 0.0f;
      const float skid_p = 0.85f + 0.30f * std::clamp(max_slip, 0.0f, 1.0f);
      mix_loop(tire_skid_voice_, skid_id, skid_g, skid_p, 0.707f, 0.707f, tyre_bus, frames);
    }
  }

  // 3. Transmission whine + shifts, Turbo spool + BOV, Supercharger, Backfires (Bus::kEngine)
  if (float* eng_bus = buses[int(Bus::kEngine)]) {
    const float rpm_span = std::max(500.0f, st.redline_rpm - st.idle_rpm);
    const float norm_rpm = std::clamp((st.rpm - st.idle_rpm) / rpm_span, 0.0f, 1.2f);

    // Transmission whine + shift start/complete.
    const float trans_g = (0.04f + 0.06f * float(std::clamp(st.trans_level, 0, 3))) * std::clamp(norm_rpm, 0.0f, 1.0f) *
                          (st.speed_mph > 2.0f ? 1.0f : 0.0f);
    const float trans_p = 0.55f + 1.15f * std::clamp(norm_rpm, 0.0f, 1.2f);
    mix_loop(trans_voice_, SfxSoundId::kTransWhine, trans_g, trans_p, 0.707f, 0.707f, eng_bus, frames);
    if (st.shifting && !prev_shifting_) spawn_one_shot(SfxSoundId::kShiftStart, Bus::kEngine, 0.35f, 0.707f, 0.707f);
    if (!st.shifting && prev_shifting_) spawn_one_shot(SfxSoundId::kShiftComplete, Bus::kEngine, 0.30f, 0.707f, 0.707f);
    prev_shifting_ = st.shifting;

    // Turbo spool & BOV (4.5 / 13.0 PSI thresholds).
    const float turbo_g = st.forced_induction == 1 ? 0.35f * std::clamp(st.turbo_norm_rpm, 0.0f, 1.0f) : 0.0f;
    const float turbo_p = 0.60f + 0.95f * std::clamp(st.turbo_norm_rpm, 0.0f, 1.2f);
    mix_loop(turbo_voice_, SfxSoundId::kTurboSpool, turbo_g, turbo_p, 0.707f, 0.707f, eng_bus, frames);
    if (st.forced_induction == 1 && st.bov_open && !prev_bov_open_) {
      const BovVariant v = select_bov_variant(st.turbo_psi);
      if (v == BovVariant::kHi) spawn_one_shot(SfxSoundId::kTurboBovHi, Bus::kEngine, 0.55f, 0.707f, 0.707f);
      else if (v == BovVariant::kLo) spawn_one_shot(SfxSoundId::kTurboBovLo, Bus::kEngine, 0.45f, 0.707f, 0.707f);
    }
    prev_bov_open_ = st.bov_open;

    // Supercharger whine.
    const float sc_g = st.forced_induction == 2
                           ? 0.40f * std::clamp(st.supercharger_norm_rpm, 0.0f, 1.0f) * (0.4f + 0.6f * st.throttle)
                           : 0.0f;
    const float sc_p = 0.55f + 1.10f * std::clamp(st.supercharger_norm_rpm, 0.0f, 1.3f);
    mix_loop(sc_voice_, SfxSoundId::kScWhine, sc_g, sc_p, 0.707f, 0.707f, eng_bus, frames);

    // Exhaust backfire / overrun burble timer + Expander coupling.
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
            const float bg = 0.60f * float(lv.volume.present ? lv.volume.eval({norm_rpm, 0, 0, 1}) : 0.7);
            spawn_one_shot(SfxSoundId::kBackfirePop, Bus::kEngine, bg, 0.707f, 0.707f);
            spawn_one_shot(SfxSoundId::kBackfireLfe, Bus::kEngine, bg * 0.65f, 0.707f, 0.707f);
            backfire_count_.fetch_add(1, std::memory_order_relaxed);
            if (focus_engine) focus_engine->trigger_expander();
          }
        }
      }
    }
  }

  // 4. 3D spatialized AI opponent & traffic car engines (Bus::kOtherCars)
  if (float* other_bus = buses[int(Bus::kOtherCars)]) {
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
      const SfxSoundId sid = car.is_traffic ? SfxSoundId::kTrafficCarLoop : SfxSoundId::kOtherCarLoop;
      mix_loop(other_voices_[size_t(i)], sid, 0.55f * atten, base_pitch * doppler, pl, pr, other_bus, frames);
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
