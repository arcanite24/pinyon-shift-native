#include "audio/audio_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include "audio/bake.h"
#include "audio/fsb4.h"
#include "audio/xma_decode.h"
#include "cars/mods.h"

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#if defined(__SSE3__) || defined(_M_X64) || defined(_MSC_VER)
#include <pmmintrin.h>
#endif
#endif

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

inline float safe_f(float v, float fallback = 0.0f) { return std::isfinite(v) ? v : fallback; }
inline float safe_clamp_f(float v, float lo, float hi, float fallback = 0.0f) {
  return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
inline double safe_clamp_d(double v, double lo, double hi, double fallback = 0.0) {
  return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

void sanitize_engine_update(EnginePlayer::Update& u) {
  u.rpm = safe_clamp_d(u.rpm, 0.0, 32000.0, 0.0);
  u.throttle = safe_clamp_d(u.throttle, 0.0, 1.0, 0.0);
  u.pos_torque = safe_clamp_d(u.pos_torque, 0.0, 1.0, 0.0);
  u.neg_torque = safe_clamp_d(u.neg_torque, -1.0, 1.0, 0.0);
}

void sanitize_vec3(Vec3f& v, float limit = 20000.0f) {
  v.x = safe_clamp_f(v.x, -limit, limit, 0.0f);
  v.y = safe_clamp_f(v.y, -limit, limit, 0.0f);
  v.z = safe_clamp_f(v.z, -limit, limit, 0.0f);
}

void sanitize_vehicle_state(VehicleAudioState& st) {
  st.speed_mph = safe_clamp_f(st.speed_mph, 0.0f, 400.0f, 0.0f);
  st.top_speed_mph = safe_clamp_f(st.top_speed_mph, 40.0f, 400.0f, 140.0f);
  st.rpm = safe_clamp_f(st.rpm, 0.0f, 32000.0f, 800.0f);
  st.idle_rpm = safe_clamp_f(st.idle_rpm, 100.0f, 5000.0f, 800.0f);
  st.redline_rpm = safe_clamp_f(st.redline_rpm, st.idle_rpm + 500.0f, 32000.0f, 7500.0f);
  st.throttle = safe_clamp_f(st.throttle, 0.0f, 1.0f, 0.0f);
  st.clutch_pos = safe_clamp_f(st.clutch_pos, 0.0f, 1.0f, 0.0f);
  st.turbo_psi = safe_clamp_f(st.turbo_psi, -15.0f, 60.0f, 0.0f);
  st.turbo_norm_rpm = safe_clamp_f(st.turbo_norm_rpm, 0.0f, 1.5f, 0.0f);
  st.supercharger_norm_rpm = safe_clamp_f(st.supercharger_norm_rpm, 0.0f, 1.5f, 0.0f);
  st.scrape_intensity = safe_clamp_f(st.scrape_intensity, 0.0f, 1.0f, 0.0f);
  st.reflection_proximity = safe_clamp_f(st.reflection_proximity, 0.0f, 1.0f, 0.0f);
  st.active_aero_deploy = safe_clamp_f(st.active_aero_deploy, 0.0f, 1.0f, 0.0f);
  st.engine_damage = safe_clamp_f(st.engine_damage, 0.0f, 1.0f, 0.0f);
  st.drivetrain_damage = safe_clamp_f(st.drivetrain_damage, 0.0f, 1.0f, 0.0f);
  st.burble_vol_scale = safe_clamp_f(st.burble_vol_scale, 0.0f, 2.5f, 1.0f);
  st.exh_noise_vol_scale = safe_clamp_f(st.exh_noise_vol_scale, 0.0f, 2.5f, 1.0f);
  st.view_wind_scale = safe_clamp_f(st.view_wind_scale, 0.0f, 2.5f, 1.0f);
  st.view_tyre_scale = safe_clamp_f(st.view_tyre_scale, 0.0f, 2.5f, 1.0f);
  st.view_trans_scale = safe_clamp_f(st.view_trans_scale, 0.0f, 2.5f, 1.0f);
  st.view_turbo_scale = safe_clamp_f(st.view_turbo_scale, 0.0f, 2.5f, 1.0f);
  st.view_sc_scale = safe_clamp_f(st.view_sc_scale, 0.0f, 2.5f, 1.0f);
  st.view_backfire_scale = safe_clamp_f(st.view_backfire_scale, 0.0f, 2.5f, 1.0f);
  st.view_burble_scale = safe_clamp_f(st.view_burble_scale, 0.0f, 2.5f, 1.0f);
  st.view_susp_scale = safe_clamp_f(st.view_susp_scale, 0.0f, 2.5f, 1.0f);
  st.view_ambience_scale = safe_clamp_f(st.view_ambience_scale, 0.0f, 2.5f, 1.0f);
  st.view_other_cars_scale = safe_clamp_f(st.view_other_cars_scale, 0.0f, 2.5f, 1.0f);
  st.view_damage_scale = safe_clamp_f(st.view_damage_scale, 0.0f, 2.5f, 1.0f);
  st.view_exh_noise_scale = safe_clamp_f(st.view_exh_noise_scale, 0.0f, 2.5f, 1.0f);
  st.view_ev_scale = safe_clamp_f(st.view_ev_scale, 0.0f, 2.5f, 1.0f);
  st.view_lfe_scale = safe_clamp_f(st.view_lfe_scale, 0.0f, 2.5f, 1.0f);
  st.view_refl_scale = safe_clamp_f(st.view_refl_scale, 0.0f, 2.5f, 1.0f);
  for (auto& w : st.wheels) {
    w.slip_combined = safe_clamp_f(w.slip_combined, 0.0f, 4.0f, 0.0f);
    w.wheel_rpm = safe_clamp_f(w.wheel_rpm, -10000.0f, 10000.0f, 0.0f);
    w.load = safe_clamp_f(w.load, 0.0f, 4.0f, 0.0f);
    w.bottom_out_force = safe_clamp_f(w.bottom_out_force, 0.0f, 10.0f, 0.0f);
  }
  sanitize_vec3(st.listener_pos, 20000.0f);
  sanitize_vec3(st.listener_vel, 300.0f);
  sanitize_vec3(st.listener_right, 2.0f);
  for (auto& oc : st.other_cars) {
    if (!oc.active) continue;
    sanitize_vec3(oc.pos, 20000.0f);
    sanitize_vec3(oc.vel, 300.0f);
    oc.rpm = safe_clamp_f(oc.rpm, 0.0f, 32000.0f, 1000.0f);
    oc.speed_mph = safe_clamp_f(oc.speed_mph, 0.0f, 400.0f, 0.0f);
  }
  if (st.showcase.active) {
    sanitize_vec3(st.showcase.pos, 20000.0f);
    sanitize_vec3(st.showcase.vel, 400.0f);
    st.showcase.speed_mps = safe_clamp_f(st.showcase.speed_mps, 0.0f, 400.0f, 0.0f);
    st.showcase.bank = safe_clamp_f(st.showcase.bank, -3.15f, 3.15f, 0.0f);
    st.showcase.pitch = safe_clamp_f(st.showcase.pitch, -1.58f, 1.58f, 0.0f);
  }
}

void enable_audio_thread_rt_mode() {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#if defined(_MM_SET_DENORMALS_ZERO_MODE)
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
#endif
#ifdef _WIN32
  // Keep audio synthesis ahead of background world texture/mesh decoding bursts
  // so 200 m streaming rings cannot starve WASAPI and trigger crackling underruns.
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  }
#endif
}

}  // namespace

AudioEngine::AudioEngine(int rate) : rate_(std::max(8000, rate)), ambience_bed_(std::max(8000, rate)) {
  mixer_.prepare(rate_);
  mixer_.snapshots().add_defaults();
  snap_title_ = mixer_.snapshots().find("Title");
  {
    Snapshot dd;
    dd.name = "DialogDuck";
    dd.dest[int(Bus::kMusic)] = 0.25f;
    dd.dest[int(Bus::kEngine)] = 0.50f;
    dd.dest[int(Bus::kOtherCars)] = 0.50f;
    dd.dest[int(Bus::kTyres)] = 0.60f;
    dd.dest[int(Bus::kWind)] = 0.55f;
    dd.dest[int(Bus::kAmbience)] = 0.45f;
    dd.fade = 0.18f;
    dd.return_fade = 0.35f;
    snap_dialog_duck_ = mixer_.snapshots().add(std::move(dd));
  }
  for (auto& eq : car_eqs_) eq.prepare(rate_);
  rms_stage_.prepare(rate_);
  reverb_.prepare(rate_);
  matrix_bus_smooth_.fill(1.0f);
  for (auto& s : bus_levels_shadow_) s.store(1.0f, std::memory_order_relaxed);
  // Pre-allocate stereo bus scratch buffers + mono LFE buffer for up to 4096 frames
  // so the audio thread never allocates in steady state.
  for (auto& b : bus_bufs_) b.resize(4096 * 2, 0.0f);
  lfe_buf_.resize(4096, 0.0f);
  reverb_send_buf_.resize(4096 * 2, 0.0f);
  reverb_wet_buf_.resize(4096 * 2, 0.0f);
  out_buf_.resize(4096 * 2, 0.0f);
}

AudioEngine::~AudioEngine() {
  stop_ui_music_worker();
  stop();
  drain_ui_music_loaded();
  for (auto*& t : ui_tracks_) {
    delete t;
    t = nullptr;
  }
  delete engine_;
  delete sfx_;
  delete satnav_;
  delete codriver_;
  delete vo_;
  delete radio_player_;
  delete runtime_radio_;
  delete user_music_;
}

void AudioEngine::stop_ui_music_worker() {
  stop_ui_music_.store(true, std::memory_order_release);
  if (ui_music_worker_.joinable()) ui_music_worker_.join();
  stop_ui_music_.store(false, std::memory_order_release);
}

void AudioEngine::drain_ui_music_loaded() {
  LoadedUiTrack lt{};
  while (ui_music_q_.pop(lt)) {
    const size_t idx = size_t(lt.mode);
    if (idx < ui_tracks_.size()) {
      delete ui_tracks_[idx];
      ui_tracks_[idx] = lt.track;
    } else {
      delete lt.track;
    }
  }
}

void AudioEngine::init_ui_music(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir) {
  stop_ui_music_worker();
  if (game_root.empty()) return;
  const auto bank_path = cars::mods::resolve(game_root / "media" / "audio" / "UI" / "UIInGame" / "UIInGame_Streams.fsb");
  std::error_code ec;
  if (!std::filesystem::exists(bank_path, ec)) return;
  stop_ui_music_.store(false, std::memory_order_release);
  ui_music_worker_ = std::thread([this, bank_path, cache_dir]() {
    std::ifstream bf(bank_path, std::ios::binary);
    if (!bf) return;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    std::vector<FsbSample> samples;
    if (!read_fsb4(raw, samples, nullptr) || samples.empty()) return;
    auto find_sub = [&](std::string_view needle) -> int {
      std::string want(needle);
      for (char& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
      for (size_t i = 0; i < samples.size(); ++i) {
        std::string nm = samples[i].name;
        for (char& c : nm) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (nm.find(want) != std::string::npos) return int(i);
      }
      return -1;
    };
    struct Job {
      UiMusicMode mode;
      const char* name;
      const char* cache_key;
    };
    static constexpr Job kJobs[] = {
        {UiMusicMode::kTitle, "pressstart_music", "ui_title"},
        {UiMusicMode::kHomespace, "multiplayer_autoshow_loop", "ui_homespace"},
        {UiMusicMode::kLoading, "01_loadingscreen_band", "ui_loading"},
        {UiMusicMode::kMap, "map_ambientbackgroundloop", "ui_map"},
        {UiMusicMode::kPostRaceVictory, "cs_postrace_victory_cutscene", "ui_victory"},
    };
    const auto ff = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
    const auto music_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "uimusic");
    std::error_code tec;
    if (!music_cache.empty()) std::filesystem::create_directories(music_cache, tec);
    for (const auto& j : kJobs) {
      if (stop_ui_music_.load(std::memory_order_acquire)) break;
      const int sub = find_sub(j.name);
      if (sub < 0) continue;
      auto tr = std::make_unique<StereoTrack>();
      bool ok = false;
      const auto cf = music_cache.empty() ? std::filesystem::path{} : (music_cache / (std::string(j.cache_key) + ".s16st"));
      if (!cf.empty()) {
        std::ifstream cf_in(cf, std::ios::binary);
        int32_t r = 0;
        uint32_t n = 0;
        if (cf_in.read(reinterpret_cast<char*>(&r), 4) && cf_in.read(reinterpret_cast<char*>(&n), 4) &&
            r > 0 && n > 0 && n <= 48000u * 600u) {
          std::vector<int16_t> s16(size_t(n) * 2);
          if (cf_in.read(reinterpret_cast<char*>(s16.data()), std::streamsize(s16.size() * 2))) {
            tr->rate = r;
            tr->stereo.resize(s16.size());
            for (size_t i = 0; i < s16.size(); ++i) tr->stereo[i] = float(s16[i]) / 32768.0f;
            ok = true;
          }
        }
      }
      if (!ok) {
        ok = decode_fsb_subsound_stereo(bank_path, uint32_t(sub), tr->stereo, tr->rate, ff, {});
        if (ok && !cf.empty() && !tr->stereo.empty()) {
          std::ofstream cf_out(cf, std::ios::binary | std::ios::trunc);
          if (cf_out) {
            const int32_t r = tr->rate;
            const uint32_t n = uint32_t(tr->stereo.size() / 2);
            cf_out.write(reinterpret_cast<const char*>(&r), 4);
            cf_out.write(reinterpret_cast<const char*>(&n), 4);
            std::vector<int16_t> s16(tr->stereo.size());
            for (size_t i = 0; i < tr->stereo.size(); ++i)
              s16[i] = int16_t(std::clamp(std::lround(double(tr->stereo[i]) * 32767.0), -32768l, 32767l));
            cf_out.write(reinterpret_cast<const char*>(s16.data()), std::streamsize(s16.size() * 2));
          }
        }
      }
      if (ok && !tr->stereo.empty()) {
        StereoTrack* raw_t = tr.release();
        if (!ui_music_q_.push({j.mode, raw_t})) delete raw_t;
      }
    }
  });
}

void AudioEngine::set_ui_music(UiMusicMode mode) {
  Command c;
  c.kind = Command::kUiMusicMode;
  c.i0 = int(mode);
  enqueue(c);
}

void AudioEngine::set_homespace_mode(int mode) {
  Command c;
  c.kind = Command::kHomespaceMode;
  c.i0 = std::clamp(mode, 0, 3);
  enqueue(c);
}

void AudioEngine::set_master_dsp_config(const MasterDspConfig& cfg) {
  if (!running_.load(std::memory_order_acquire)) {
    rms_stage_.set_config(cfg);
  }
}

void AudioEngine::set_playback_system(PlaybackSystem sys) {
  Command c;
  c.kind = Command::kPlaybackSystem;
  c.i0 = int(sys);
  enqueue(c);
}

void AudioEngine::set_playback_system(bool tv_profile) {
  set_playback_system(tv_profile ? PlaybackSystem::kTv : PlaybackSystem::kHomeTheater);
}

void AudioEngine::start(Sink sink) {
  stop();
  sink_ = std::move(sink);
  sink_.block_frames = std::clamp(sink_.block_frames, 64, 4096);
  sink_.target_buffer_frames = std::max(sink_.block_frames, sink_.target_buffer_frames);
  recovery_fade_pos_ = 0;
  recovery_fade_len_ = 0;
  adaptive_extra_frames_ = 0;
  clean_blocks_since_underrun_ = 0;
  stop_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { thread_main(); });
}

void AudioEngine::stop() {
  if (running_.exchange(false, std::memory_order_acq_rel)) {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }
  drain_commands();
  reclaim();
}

void AudioEngine::reclaim() {
  ambience_bed_.reclaim_retired();
  Retired r;
  while (retired_.pop(r)) {
    switch (r.kind) {
      case Retired::kEngine: delete static_cast<EnginePlayer*>(r.ptr); break;
      case Retired::kSfx: {
        std::lock_guard<std::mutex> lk(sfx_reload_mu_);
        if (sfx_control_ptr_ == r.ptr) sfx_control_ptr_ = nullptr;
        delete static_cast<SfxEngine*>(r.ptr);
        break;
      }
      case Retired::kSatNav: delete static_cast<SatNavVoPlayer*>(r.ptr); break;
      case Retired::kCoDriver: delete static_cast<CoDriverVoPlayer*>(r.ptr); break;
      case Retired::kVo: delete static_cast<VoPlayer*>(r.ptr); break;
      case Retired::kRadio: delete static_cast<RadioPlayer*>(r.ptr); break;
      case Retired::kRuntimeRadio: delete static_cast<RuntimeRadioPlayer*>(r.ptr); break;
      case Retired::kUserMusic: delete static_cast<UserMusicPlayer*>(r.ptr); break;
    }
  }
}

void AudioEngine::enqueue(const Command& c) {
  reclaim();
  if (!running_.load(std::memory_order_acquire)) {
    // When not running a render thread, still push through the queue (or
    // drain if nearly full) so offline renders and pre-start setup share one
    // path.
    if (!cmds_.push(c)) {
      drain_commands();
      reclaim();
      cmds_.push(c);
    }
    return;
  }
  // Never block the main thread; if the queue is saturated, drop non-owning
  // parameter updates (swaps should never saturate a 512-entry queue).
  cmds_.push(c);
}

void AudioEngine::set_engine_input(const EnginePlayer::Update& u) { engine_in_.write(u); }
void AudioEngine::set_vehicle_state(const VehicleAudioState& st) { veh_state_in_.write(st); }

void AudioEngine::set_engine_player(std::unique_ptr<EnginePlayer> player) {
  reclaim();
  Command c;
  c.kind = Command::kSwapEngine;
  c.ptr0 = player.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<EnginePlayer*>(c.ptr0);
  }
}

void AudioEngine::set_sfx_engine(std::unique_ptr<SfxEngine> sfx) {
  reclaim();
  Command c;
  c.kind = Command::kSwapSfx;
  SfxEngine* raw = sfx.release();
  c.ptr0 = raw;
  {
    std::lock_guard<std::mutex> lk(sfx_reload_mu_);
    sfx_control_ptr_ = raw;
  }
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    {
      std::lock_guard<std::mutex> lk(sfx_reload_mu_);
      if (sfx_control_ptr_ == raw) sfx_control_ptr_ = nullptr;
    }
    delete raw;
  }
}

void AudioEngine::set_satnav_vo(std::unique_ptr<SatNavVoPlayer> satnav) {
  Command c;
  c.kind = Command::kSwapSatNav;
  c.ptr0 = satnav.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<SatNavVoPlayer*>(c.ptr0);
  }
}

void AudioEngine::trigger_collision(const CollisionHit& hit) {
  Command c;
  c.kind = Command::kTriggerCollision;
  c.f0 = hit.rel_speed_mps;
  c.f1 = hit.pos.x;
  c.f2 = hit.pos.y;
  c.f3 = hit.pos.z;
  c.b0 = hit.is_car_on_car;
  c.b1 = hit.is_break;
  c.i0 = int(hit.surface_hint);
  c.i1 = int(hit.smash_material);
  enqueue(c);
}

void AudioEngine::trigger_landing(const LandingHit& hit) {
  Command c;
  c.kind = Command::kTriggerLanding;
  c.f0 = hit.impact_speed_mps;
  c.f1 = hit.pos.x;
  c.f2 = hit.pos.y;
  c.f3 = hit.pos.z;
  c.i0 = int(hit.surface_hint);
  c.i1 = int(std::lround(std::clamp(std::isfinite(hit.air_time_s) ? hit.air_time_s : 0.0f, 0.0f, 30.0f) * 1000.0f));
  enqueue(c);
}

void AudioEngine::trigger_ui(UiCue cue) {
  Command c;
  c.kind = Command::kTriggerUi;
  c.i0 = int(cue);
  enqueue(c);
}

void AudioEngine::trigger_ui_event(const UiAudioCatalog::Event& event, float gain) {
  // One command per layer: the audio thread drains them in the same block, so
  // the layers start on the same frame.
  for (const auto& l : event.layers) {
    if (l.sample < 0) continue;
    Command c;
    c.kind = Command::kTriggerUiEvent;
    c.i0 = l.sample;
    c.f0 = l.gain * gain;
    c.f1 = l.left;
    c.f2 = l.right;
    enqueue(c);
  }
}

void AudioEngine::trigger_satnav(SatNavCue cue) {
  Command c;
  c.kind = Command::kTriggerSatNav;
  c.i0 = int(cue);
  enqueue(c);
}

void AudioEngine::set_satnav_enabled(bool on) {
  Command c;
  c.kind = Command::kSatNavEnable;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::set_radio(Radio* radio, std::unique_ptr<RadioPlayer> player) {
  Command c;
  c.kind = Command::kSwapRadio;
  c.ptr0 = radio;
  c.ptr1 = player.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<RadioPlayer*>(c.ptr1);
  }
}

void AudioEngine::set_runtime_radio(std::unique_ptr<RuntimeRadioPlayer> player) {
  Command c;
  c.kind = Command::kSwapRuntimeRadio;
  c.ptr0 = player.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<RuntimeRadioPlayer*>(c.ptr0);
  }
}

void AudioEngine::set_user_music(std::unique_ptr<UserMusicPlayer> player) {
  Command c;
  c.kind = Command::kSwapUserMusic;
  c.ptr0 = player.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<UserMusicPlayer*>(c.ptr0);
  }
}

void AudioEngine::set_fmv_audio(FmvAudioFn fn, void* ctx) {
  if (!fn) {
    fmv_fn_.store(nullptr, std::memory_order_release);
    fmv_ctx_.store(nullptr, std::memory_order_release);
  } else {
    fmv_ctx_.store(ctx, std::memory_order_release);
    fmv_fn_.store(fn, std::memory_order_release);
  }
}

void AudioEngine::set_vo_player(std::unique_ptr<VoPlayer> vo) {
  Command c;
  c.kind = Command::kSwapVo;
  c.ptr0 = vo.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<VoPlayer*>(c.ptr0);
  }
}

void AudioEngine::set_codriver_vo(std::unique_ptr<CoDriverVoPlayer> codriver) {
  Command c;
  c.kind = Command::kSwapCoDriver;
  c.ptr0 = codriver.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<CoDriverVoPlayer*>(c.ptr0);
  }
}

void AudioEngine::trigger_codriver(std::string_view cue_path, bool supersede) {
  Command c;
  c.kind = Command::kTriggerCoDriver;
  c.b0 = supersede;
  const size_t n = std::min(cue_path.size(), c.text.size() - 1);
  std::memcpy(c.text.data(), cue_path.data(), n);
  c.text[n] = '\0';
  enqueue(c);
}

void AudioEngine::clear_codriver() {
  Command c;
  c.kind = Command::kClearCoDriver;
  enqueue(c);
}

void AudioEngine::set_acoustics_sample(const AcousticsSample& sample) {
  acoustics_in_.write(RtAcousticsSample::from_sample(sample));
}

void AudioEngine::set_listener_pose(std::array<float, 3> pos, float heading_rad) {
  Command c;
  c.kind = Command::kListenerPose;
  c.f0 = pos[0];
  c.f1 = pos[1];
  c.f2 = pos[2];
  c.f3 = heading_rad;
  enqueue(c);
}

void AudioEngine::trigger_vo(std::string_view trigger_id, bool interrupt, int variant) {
  Command c;
  c.kind = Command::kTriggerVo;
  c.i0 = variant;
  c.b0 = interrupt;
  const size_t n = std::min(trigger_id.size(), c.text.size() - 1);
  std::memcpy(c.text.data(), trigger_id.data(), n);
  c.text[n] = '\0';
  enqueue(c);
}

void AudioEngine::skip_vo() {
  Command c;
  c.kind = Command::kSkipVo;
  enqueue(c);
}

void AudioEngine::set_dialog_ducking(bool active) {
  Command c;
  c.kind = Command::kDialogDuck;
  c.b0 = active;
  enqueue(c);
}

void AudioEngine::radio_set_time_of_day(double hour) {
  Command c;
  c.kind = Command::kRadioTod;
  c.f0 = float(hour);
  enqueue(c);
}

void AudioEngine::radio_trigger_special(std::string_view gameplay_event) {
  Command c;
  c.kind = Command::kRadioSpecial;
  const size_t n = std::min(gameplay_event.size(), c.text.size() - 1);
  std::memcpy(c.text.data(), gameplay_event.data(), n);
  c.text[n] = '\0';
  enqueue(c);
}

void AudioEngine::radio_trigger_immediate(std::string_view gameplay_event) {
  Command c;
  c.kind = Command::kRadioImmediate;
  const size_t n = std::min(gameplay_event.size(), c.text.size() - 1);
  std::memcpy(c.text.data(), gameplay_event.data(), n);
  c.text[n] = '\0';
  enqueue(c);
}

void AudioEngine::radio_cancel_festival_updates() {
  Command c;
  c.kind = Command::kRadioCancelFestivalUpdates;
  enqueue(c);
}

void AudioEngine::radio_trigger_festival_update(std::string_view gameplay_event) {
  Command c;
  c.kind = Command::kRadioFestivalUpdate;
  const size_t n = std::min(gameplay_event.size(), c.text.size() - 1);
  std::memcpy(c.text.data(), gameplay_event.data(), n);
  c.text[n] = '\0';
  enqueue(c);
}

void AudioEngine::radio_lock_viper_intro(bool lock) {
  Command c;
  c.kind = Command::kRadioViperLock;
  c.b0 = lock;
  enqueue(c);
}

namespace {
enum RadioFlowOp { kFlowPlaylist, kFlowSwitching, kFlowSilentIdents, kFlowStopDj, kFlowCancelPlaylist, kFlowHoldPopup };
}  // namespace

void AudioEngine::radio_set_playlist(const std::vector<std::string>& fmodnames, bool then_normal) {
  Command c;
  c.kind = Command::kRadioFlow;
  c.i0 = kFlowPlaylist;
  c.b0 = then_normal;
  std::string joined;
  for (const auto& n : fmodnames) {
    if (!joined.empty()) joined += '|';
    joined += n;
  }
  if (joined.size() >= c.text.size()) return;  // never a truncated name
  std::memcpy(c.text.data(), joined.data(), joined.size());
  c.text[joined.size()] = '\0';
  enqueue(c);
}

void AudioEngine::radio_set_switching(bool on) {
  Command c;
  c.kind = Command::kRadioFlow;
  c.i0 = kFlowSwitching;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::radio_set_silent_idents(bool on) {
  Command c;
  c.kind = Command::kRadioFlow;
  c.i0 = kFlowSilentIdents;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::radio_stop_dj() {
  Command c;
  c.kind = Command::kRadioFlow;
  c.i0 = kFlowStopDj;
  enqueue(c);
}

void AudioEngine::radio_cancel_playlist() {
  Command c;
  c.kind = Command::kRadioFlow;
  c.i0 = kFlowCancelPlaylist;
  enqueue(c);
}

void AudioEngine::radio_hold_popup(bool held) {
  Command c;
  c.kind = Command::kRadioFlow;
  c.i0 = kFlowHoldPopup;
  c.b0 = held;
  enqueue(c);
}

void AudioEngine::set_bus_level(Bus b, float level) {
  const float clamped = std::clamp(level, 0.0f, 2.0f);
  if (int(b) >= 0 && int(b) < kBuses) {
    bus_levels_shadow_[size_t(b)].store(clamped, std::memory_order_release);
    if (int(b) < kUiSliderBuses) {
      for (int child = kUiSliderBuses; child < kBuses; ++child) {
        if (bus_parent(Bus(child)) == b) {
          bus_levels_shadow_[size_t(child)].store(clamped, std::memory_order_release);
        }
      }
    }
  }
  Command c;
  c.kind = Command::kBusLevel;
  c.i0 = int(b);
  c.f0 = clamped;
  enqueue(c);
}

float AudioEngine::bus_level(Bus b) const {
  if (int(b) < 0 || int(b) >= kBuses) return 1.0f;
  return bus_levels_shadow_[size_t(b)].load(std::memory_order_acquire);
}

void AudioEngine::set_master_volume(float volume) {
  Command c;
  c.kind = Command::kMasterVolume;
  c.f0 = volume;
  enqueue(c);
}

void AudioEngine::set_mute(bool mute) {
  Command c;
  c.kind = Command::kMute;
  c.b0 = mute;
  enqueue(c);
}

void AudioEngine::set_full_dynamic_range(bool on) {
  Command c;
  c.kind = Command::kFullDynamicRange;
  c.b0 = on;
  enqueue(c);
}

int AudioEngine::register_snapshot(Snapshot snap) {
  if (running_.load(std::memory_order_acquire)) return -1;
  return mixer_.snapshots().add(std::move(snap));
}

int AudioEngine::find_snapshot(std::string_view name) const { return mixer_.snapshots().find(name); }

void AudioEngine::set_snapshot(int id, bool active) {
  if (id < 0) return;
  Command c;
  c.kind = Command::kSnapshot;
  c.i0 = id;
  c.b0 = active;
  enqueue(c);
}

bool AudioEngine::load_snapshot_matrix_xml(std::string_view xml, std::string* error) {
  if (running_.load(std::memory_order_acquire)) return false;
  if (!matrix_.parse(xml, error)) return false;
  use_matrix_ = true;
  for (int b = 0; b < kBuses; ++b) matrix_bus_smooth_[size_t(b)] = matrix_.bus_gain(Bus(b));
  return true;
}

int AudioEngine::load_ambience_beds(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                                    const std::filesystem::path& state_root) {
  int total = ambience_bed_.load_from_disc(game_root, cache_dir);
  if (!state_root.empty()) {
    total += ambience_bed_.load_rally_from_disc(state_root, cache_dir);
  }
  return total;
}

int AudioEngine::want_ambience_events(const std::vector<uint32_t>& event_hashes) {
  return ambience_bed_.want_events(event_hashes);
}

int AudioEngine::load_rally_ambience_beds(const std::filesystem::path& rally_root_or_state_root,
                                          const std::filesystem::path& cache_dir) {
  return ambience_bed_.load_rally_from_disc(rally_root_or_state_root, cache_dir);
}

void AudioEngine::set_rally_world(bool on) {
  ambience_bed_.set_rally_world(on);
  Command c;
  c.kind = Command::kRallyWorld;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::set_ambience_state(float in_race, float pre_race_ambience, float camera_type,
                                     float crowd_excitement) {
  Command c;
  c.kind = Command::kAmbienceState;
  c.f0 = in_race;
  c.f1 = pre_race_ambience;
  c.f2 = camera_type;
  c.f3 = crowd_excitement;
  enqueue(c);
}

void AudioEngine::set_ambience_bed_sample(AmbienceBedSlot slot, PcmSample pcm) {
  ambience_bed_.set_bed_sample(slot, std::move(pcm));
}

void AudioEngine::set_rally_ambience_bed_sample(int region_idx, std::string_view region_name, PcmSample pcm) {
  ambience_bed_.set_rally_bed_sample(region_idx, region_name, std::move(pcm));
}

bool AudioEngine::load_rally_vo(const std::filesystem::path& rally_root_or_state_root, std::string_view language,
                                std::string* note) {
  if (running_.load(std::memory_order_acquire)) {
    // Build and load off the audio thread, preserving base VO when known, then
    // hand off atomically via the lock-free SPSC swap queue.
    auto next = std::make_unique<VoPlayer>(rate_, true);
    if (!vo_base_game_root_.empty()) {
      next->load(vo_base_game_root_, language, nullptr);
    }
    if (!next->load_rally_vo(rally_root_or_state_root, language, note)) {
      return false;
    }
    set_vo_player(std::move(next));
    return true;
  }
  drain_commands();
  if (!vo_) {
    vo_ = new VoPlayer(rate_, true);
  }
  return vo_->load_rally_vo(rally_root_or_state_root, language, note);
}

void AudioEngine::reload_sfx_burble(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                                    const std::string& burble_bank, int burble_count, int backfire_count) {
  if (!running_.load(std::memory_order_acquire)) drain_commands();
  std::lock_guard<std::mutex> lk(sfx_reload_mu_);
  SfxEngine* target = sfx_control_ptr_ ? sfx_control_ptr_ : (!running_.load(std::memory_order_acquire) ? sfx_ : nullptr);
  if (target) target->reload_burble_bank(game_root, cache_dir, burble_bank, burble_count, backfire_count);
}

void AudioEngine::set_other_car_engine(uint32_t car_id, std::unique_ptr<EnginePlayer> player) {
  if (!running_.load(std::memory_order_acquire)) drain_commands();
  std::lock_guard<std::mutex> lk(sfx_reload_mu_);
  SfxEngine* target = sfx_control_ptr_ ? sfx_control_ptr_ : (!running_.load(std::memory_order_acquire) ? sfx_ : nullptr);
  if (target) target->set_other_car_engine(car_id, std::move(player));
}

void AudioEngine::set_other_car_engines_owned(bool on) {
  if (!running_.load(std::memory_order_acquire)) drain_commands();
  std::lock_guard<std::mutex> lk(sfx_reload_mu_);
  SfxEngine* target = sfx_control_ptr_ ? sfx_control_ptr_ : (!running_.load(std::memory_order_acquire) ? sfx_ : nullptr);
  if (target) target->set_other_car_engines_owned(on);
}

int AudioEngine::other_car_engine_count() {
  std::lock_guard<std::mutex> lk(sfx_reload_mu_);
  SfxEngine* target = sfx_control_ptr_ ? sfx_control_ptr_ : (!running_.load(std::memory_order_acquire) ? sfx_ : nullptr);
  return target ? target->other_car_engine_count() : 0;
}

bool AudioEngine::load_mixer_configs(const std::filesystem::path& game_root, std::string* note,
                                     const std::filesystem::path& state_root) {
  if (running_.load(std::memory_order_acquire) || game_root.empty()) return false;
  if (vo_base_game_root_.empty()) vo_base_game_root_ = game_root;
  std::string err;
  const bool have_matrix = matrix_.load(game_root, &err, state_root);
  if (have_matrix) {
    use_matrix_ = true;
    for (int b = 0; b < kBuses; ++b) matrix_bus_smooth_[size_t(b)] = matrix_.bus_gain(Bus(b));
  }
  global_dsp_.load(game_root, nullptr, state_root);
  MasterDspConfig mdsp;
  if (mdsp.load(game_root, nullptr, state_root)) rms_stage_.set_config(mdsp);
  if (note) {
    if (have_matrix) {
      *note = "audio mixer: " + std::to_string(matrix_.snapshot_count()) + " snapshots, " +
              std::to_string(matrix_.category_count()) + " categories";
    } else {
      *note = err;
    }
  }
  return have_matrix;
}

void AudioEngine::activate_matrix_snapshot(std::string_view name) {
  if (!use_matrix_) return;
  const int idx = matrix_.find(name);
  if (idx >= 0) activate_matrix_snapshot_idx(idx);
}

void AudioEngine::activate_matrix_snapshot_idx(int idx) {
  if (!use_matrix_ || idx < 0) return;
  Command c;
  c.kind = Command::kMatrixActivate;
  c.i0 = idx;
  enqueue(c);
}

void AudioEngine::restore_gameplay_snapshot() {
  if (!use_matrix_) return;
  Command c;
  c.kind = Command::kMatrixRestoreGameplay;
  enqueue(c);
}

void AudioEngine::set_dynamics_speed(float speed_mph, float top_speed_mph) {
  if (!use_matrix_) return;
  Command c;
  c.kind = Command::kDynamicsSpeed;
  c.f0 = speed_mph;
  c.f1 = top_speed_mph;
  enqueue(c);
}

void AudioEngine::set_car_bus_dsp_mode(CarDspMode mode) {
  Command c;
  c.kind = Command::kCarBusDspMode;
  c.i0 = int(mode);
  enqueue(c);
}

void AudioEngine::set_radio_rally_mode(bool on) {
  radio_rally_mode_shadow_.store(on, std::memory_order_release);
  Command c;
  c.kind = Command::kRadioRallyMode;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::radio_press(int dir) {
  Command c;
  c.kind = Command::kRadioPress;
  c.i0 = dir;
  enqueue(c);
}

void AudioEngine::radio_select(int station, bool user_change) {
  Command c;
  c.kind = Command::kRadioSelect;
  c.i0 = station;
  c.b0 = user_change;
  enqueue(c);
}

void AudioEngine::radio_set_dj(bool on) {
  Command c;
  c.kind = Command::kRadioDj;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::radio_set_volume(float volume) {
  Command c;
  c.kind = Command::kRadioVolume;
  c.f0 = volume;
  enqueue(c);
}

void AudioEngine::runtime_radio_select(int station) {
  Command c;
  c.kind = Command::kRuntimeRadioSelect;
  c.i0 = station;
  enqueue(c);
}

void AudioEngine::runtime_radio_set_volume(float volume) {
  Command c;
  c.kind = Command::kRuntimeRadioVolume;
  c.f0 = volume;
  enqueue(c);
}

void AudioEngine::runtime_radio_next() {
  Command c;
  c.kind = Command::kRuntimeRadioNext;
  enqueue(c);
}

void AudioEngine::runtime_radio_prev() {
  Command c;
  c.kind = Command::kRuntimeRadioPrev;
  enqueue(c);
}

void AudioEngine::user_music_enable(bool on) {
  Command c;
  c.kind = Command::kUserMusicEnable;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::user_music_set_shuffle(bool on) {
  Command c;
  c.kind = Command::kUserMusicShuffle;
  c.b0 = on;
  enqueue(c);
}

void AudioEngine::user_music_set_volume(float volume) {
  Command c;
  c.kind = Command::kUserMusicVolume;
  c.f0 = volume;
  enqueue(c);
}

void AudioEngine::user_music_next() {
  Command c;
  c.kind = Command::kUserMusicNext;
  enqueue(c);
}

void AudioEngine::user_music_prev() {
  Command c;
  c.kind = Command::kUserMusicPrev;
  enqueue(c);
}

AudioTelemetry AudioEngine::telemetry() {
  AudioTelemetry t{};
  telemetry_buf_.read(t);
  return t;
}

void AudioEngine::retire_ptr(Retired::Kind kind, void* ptr) {
  if (!ptr) return;
  if (!retired_.push({kind, ptr})) {
    // Fallback only if main thread never reclaimed 64 swaps in a row.
    switch (kind) {
      case Retired::kEngine: delete static_cast<EnginePlayer*>(ptr); break;
      case Retired::kSfx: delete static_cast<SfxEngine*>(ptr); break;
      case Retired::kSatNav: delete static_cast<SatNavVoPlayer*>(ptr); break;
      case Retired::kCoDriver: delete static_cast<CoDriverVoPlayer*>(ptr); break;
      case Retired::kVo: delete static_cast<VoPlayer*>(ptr); break;
      case Retired::kRadio: delete static_cast<RadioPlayer*>(ptr); break;
      case Retired::kRuntimeRadio: delete static_cast<RuntimeRadioPlayer*>(ptr); break;
      case Retired::kUserMusic: delete static_cast<UserMusicPlayer*>(ptr); break;
    }
  }
}

void AudioEngine::drain_commands() {
  Command c;
  while (cmds_.pop(c)) {
    switch (c.kind) {
      case Command::kBusLevel:
        if (c.i0 >= 0 && c.i0 < kBuses) mixer_.set_bus_level(Bus(c.i0), c.f0);
        break;
      case Command::kMasterVolume: mixer_.set_master(c.f0); break;
      case Command::kMute: mixer_.set_mute(c.b0); break;
      case Command::kFullDynamicRange:
        mixer_.stage().set_full_dynamic_range(c.b0);
        rms_stage_.set_full_dynamic_range(c.b0);
        break;
      case Command::kPlaybackSystem: {
        const auto ps = PlaybackSystem(std::clamp(c.i0, 0, int(PlaybackSystem::kFullDynamicRange)));
        rms_stage_.set_playback_system(ps);
        mixer_.stage().set_full_dynamic_range(ps == PlaybackSystem::kFullDynamicRange);
        break;
      }
      case Command::kDialogDuck:
        dialog_duck_requested_ = c.b0;
        break;
      case Command::kRadioViperLock:
        if (radio_) {
          if (c.b0) radio_->lock_viper_intro_playlist();
          else radio_->unlock_viper_intro_playlist();
        }
        break;
      case Command::kRadioFlow:
        if (!radio_) break;
        switch (c.i0) {
          case kFlowPlaylist: {
            std::vector<std::string> names;
            std::string_view all(c.text.data());
            while (!all.empty()) {
              const size_t bar = all.find('|');
              names.emplace_back(all.substr(0, bar));
              if (bar == std::string_view::npos) break;
              all.remove_prefix(bar + 1);
            }
            radio_->set_playlist(names, c.b0);
            break;
          }
          case kFlowSwitching: radio_->set_switching_enabled(c.b0); break;
          case kFlowSilentIdents: radio_->set_silent_idents(c.b0); break;
          case kFlowStopDj: radio_->stop_dj(); break;
          case kFlowCancelPlaylist: radio_->cancel_playlist(); break;
          case kFlowHoldPopup: radio_->hold_popup(c.b0); break;
          default: break;
        }
        break;
      case Command::kUiMusicMode:
        ui_music_mode_ = UiMusicMode(std::clamp(c.i0, 0, int(UiMusicMode::kPostRaceVictory)));
        break;
      case Command::kHomespaceMode:
        homespace_mode_ = std::clamp(c.i0, 0, 3);
        break;
      case Command::kRadioRallyMode:
        radio_rally_mode_ = c.b0;
        if (use_matrix_) matrix_.set_radio_rally_mode(c.b0);
        break;
      case Command::kRallyWorld:
        ambience_bed_.set_rally_world(c.b0);
        break;
      case Command::kAmbienceState: {
        auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
        ambience_params_.in_race = unit(c.f0);
        ambience_params_.pre_race_ambience = unit(c.f1);
        ambience_params_.camera_type = unit(c.f2);
        ambience_params_.crowd_excitement = unit(c.f3);
        break;
      }
      case Command::kSnapshot: mixer_.snapshots().set_active(c.i0, c.b0); break;
      case Command::kMatrixActivate:
        if (use_matrix_) {
          if (const auto* d = matrix_.def(c.i0)) matrix_.activate(d->name);
        }
        break;
      case Command::kMatrixRestoreGameplay:
        if (use_matrix_) matrix_.restore_gameplay();
        break;
      case Command::kDynamicsSpeed:
        last_veh_state_.speed_mph = c.f0;
        last_veh_state_.top_speed_mph = c.f1;
        break;
      case Command::kCarBusDspMode: {
        GlobalDspBand band{};
        switch (CarDspMode(c.i0)) {
          case CarDspMode::kRewind: band = global_dsp_.rewind; break;
          case CarDspMode::kCountdown: band = global_dsp_.countdown; break;
          case CarDspMode::kCockpit: band = global_dsp_.cockpit[0]; break;
          case CarDspMode::kOff:
          default: band.bypass = true; break;
        }
        for (auto& eq : car_eqs_) eq.set_target(band);
        break;
      }
      case Command::kRadioPress:
        if (radio_) radio_->press(c.i0);
        break;
      case Command::kRadioSelect:
        if (radio_) radio_->select(c.i0, c.b0);
        break;
      case Command::kRadioDj:
        if (radio_) radio_->set_dj_enabled(c.b0);
        break;
      case Command::kRadioVolume:
        if (radio_player_) radio_player_->volume = double(c.f0);
        break;
      case Command::kRadioTod:
        tod_hour_ = c.f0;
        if (radio_) radio_->set_time_of_day_hour(double(c.f0));
        break;
      case Command::kRadioSpecial:
        if (radio_) radio_->trigger_special_dialogue(c.text.data());
        break;
      case Command::kRadioImmediate:
        if (radio_) radio_->trigger_immediate_dialogue(c.text.data());
        break;
      case Command::kRadioFestivalUpdate:
        if (radio_) radio_->trigger_festival_update(c.text.data());
        break;
      case Command::kRadioCancelFestivalUpdates:
        if (radio_) radio_->cancel_festival_updates();
        break;
      case Command::kRuntimeRadioSelect:
        if (runtime_radio_) runtime_radio_->select_station(c.i0);
        break;
      case Command::kRuntimeRadioVolume:
        if (runtime_radio_) runtime_radio_->set_volume(c.f0);
        break;
      case Command::kRuntimeRadioNext:
        if (runtime_radio_) runtime_radio_->next_track();
        break;
      case Command::kRuntimeRadioPrev:
        if (runtime_radio_) runtime_radio_->prev_track();
        break;
      case Command::kUserMusicEnable:
        if (user_music_) user_music_->set_enabled(c.b0);
        break;
      case Command::kUserMusicShuffle:
        if (user_music_) user_music_->set_shuffle(c.b0);
        break;
      case Command::kUserMusicVolume:
        if (user_music_) user_music_->set_volume(c.f0);
        break;
      case Command::kUserMusicNext:
        if (user_music_) user_music_->next();
        break;
      case Command::kUserMusicPrev:
        if (user_music_) user_music_->prev();
        break;
      case Command::kSwapEngine:
        retire_ptr(Retired::kEngine, engine_);
        engine_ = static_cast<EnginePlayer*>(c.ptr0);
        if (engine_ && have_engine_in_) engine_->set(last_engine_in_);
        break;
      case Command::kSwapSfx:
        retire_ptr(Retired::kSfx, sfx_);
        sfx_ = static_cast<SfxEngine*>(c.ptr0);
        break;
      case Command::kSwapSatNav:
        retire_ptr(Retired::kSatNav, satnav_);
        satnav_ = static_cast<SatNavVoPlayer*>(c.ptr0);
        if (satnav_) satnav_->set_enabled(satnav_enabled_);
        break;
      case Command::kSwapCoDriver:
        retire_ptr(Retired::kCoDriver, codriver_);
        codriver_ = static_cast<CoDriverVoPlayer*>(c.ptr0);
        break;
      case Command::kSwapVo:
        retire_ptr(Retired::kVo, vo_);
        vo_ = static_cast<VoPlayer*>(c.ptr0);
        break;
      case Command::kSwapRadio:
        retire_ptr(Retired::kRadio, radio_player_);
        radio_ = static_cast<Radio*>(c.ptr0);
        radio_player_ = static_cast<RadioPlayer*>(c.ptr1);
        break;
      case Command::kSwapRuntimeRadio:
        retire_ptr(Retired::kRuntimeRadio, runtime_radio_);
        runtime_radio_ = static_cast<RuntimeRadioPlayer*>(c.ptr0);
        break;
      case Command::kSwapUserMusic:
        retire_ptr(Retired::kUserMusic, user_music_);
        user_music_ = static_cast<UserMusicPlayer*>(c.ptr0);
        break;
      case Command::kTriggerCollision:
        if (sfx_) {
          CollisionHit hit{};
          hit.rel_speed_mps = c.f0;
          hit.pos = {c.f1, c.f2, c.f3};
          hit.is_car_on_car = c.b0;
          hit.is_break = c.b1;
          hit.surface_hint = uint8_t(std::clamp(c.i0, 0, 255));
          hit.smash_material = SmashMaterial(std::clamp(c.i1, 0, int(SmashMaterial::kCount) - 1));
          sfx_->trigger_collision(hit, last_veh_state_);
        }
        break;
      case Command::kTriggerLanding:
        if (sfx_) {
          LandingHit hit{};
          hit.impact_speed_mps = c.f0;
          hit.pos = {c.f1, c.f2, c.f3};
          hit.surface_hint = uint8_t(std::clamp(c.i0, 0, 255));
          hit.air_time_s = float(std::max(0, c.i1)) * 0.001f;
          sfx_->trigger_landing(hit, last_veh_state_);
        }
        break;
      case Command::kTriggerUi:
        if (sfx_) sfx_->trigger_ui(UiCue(c.i0));
        break;
      case Command::kTriggerUiEvent:
        if (sfx_) sfx_->trigger_ui_sample(c.i0, c.f0, c.f1, c.f2);
        break;
      case Command::kTriggerSatNav:
        if (satnav_ && satnav_enabled_) satnav_->trigger(SatNavCue(c.i0));
        break;
      case Command::kSatNavEnable:
        satnav_enabled_ = c.b0;
        if (satnav_) satnav_->set_enabled(satnav_enabled_);
        break;
      case Command::kTriggerCoDriver:
        if (codriver_) codriver_->enqueue_cue(c.text.data(), c.b0);
        break;
      case Command::kClearCoDriver:
        if (codriver_) codriver_->clear();
        break;
      case Command::kTriggerVo:
        if (vo_) vo_->trigger(c.text.data(), c.b0, c.i0);
        break;
      case Command::kSkipVo:
        if (vo_) vo_->skip_current();
        break;
      case Command::kListenerPose:
        if (radio_player_) radio_player_->set_listener({c.f0, c.f1, c.f2}, c.f3);
        break;
    }
  }
}

void AudioEngine::sanitize_output(float* out, int frames) {
  if (!out || frames <= 0) return;
  bool saw_non_finite = false;
  for (int i = 0; i < frames; ++i) {
    float env = 1.0f;
    if (recovery_fade_pos_ < recovery_fade_len_) {
      // Smooth half-cosine fade-in after an underrun so recovered audio has zero click/pop.
      const float t = float(recovery_fade_pos_) / float(recovery_fade_len_);
      env = 0.5f - 0.5f * std::cos(3.14159265f * t);
      ++recovery_fade_pos_;
    }
    for (int ch = 0; ch < 2; ++ch) {
      float s = out[2 * size_t(i) + size_t(ch)];
      if (!std::isfinite(s)) {
        s = 0.0f;
        saw_non_finite = true;
      } else {
        if (std::fabs(s) < 1e-20f) s = 0.0f;
        s = std::clamp(s * env, -1.0f, 1.0f);
      }
      out[2 * size_t(i) + size_t(ch)] = s;
    }
  }
  if (saw_non_finite) {
    // Reset stateful filters/reverb/RMS stage so a bad upstream sample can never poison future blocks.
    reverb_.reset();
    rms_stage_.prepare(rate_);
    music_hp_prev_in_[0] = music_hp_prev_in_[1] = 0.0f;
    music_hp_prev_out_[0] = music_hp_prev_out_[1] = 0.0f;
    rally_hp_prev_in_[0] = rally_hp_prev_in_[1] = 0.0f;
    rally_hp_prev_out_[0] = rally_hp_prev_out_[1] = 0.0f;
    for (auto& eq : car_eqs_) eq.prepare(rate_);
  }
}

void AudioEngine::render_block(float* out, int frames) {
  if (frames <= 0) return;
  const auto t0 = std::chrono::steady_clock::now();

  VehicleAudioState vst{};
  if (veh_state_in_.read(vst)) {
    sanitize_vehicle_state(vst);
    last_veh_state_ = vst;
  }
  RtAcousticsSample ac{};
  if (acoustics_in_.read(ac)) {
    last_acoustics_ = ac;
    have_acoustics_ = true;
    reverb_.set_parameters(last_acoustics_.reverb);
  }
  drain_commands();

  const size_t samples = size_t(frames) * 2;
  if (FmvAudioFn fn = fmv_fn_.load(std::memory_order_acquire)) {
    std::fill(out, out + samples, 0.0f);
    void* ctx = fmv_ctx_.load(std::memory_order_acquire);
    const int got = std::clamp(fn(ctx, out, frames), 0, frames);
    const float g = mixer_.muted() ? 0.0f : mixer_.master();
    for (int i = 0; i < got * 2; ++i) out[i] *= g;
    sanitize_output(out, frames);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    publish_telemetry(ms, 1000.0 * double(frames) / double(rate_));
    return;
  }

  for (auto& b : bus_bufs_) {
    if (b.size() < samples) b.resize(samples, 0.0f);
  }
  if (lfe_buf_.size() < size_t(frames)) lfe_buf_.resize(size_t(frames), 0.0f);
  std::fill_n(lfe_buf_.data(), size_t(frames), 0.0f);

  EnginePlayer::Update u{};
  if (engine_in_.read(u)) {
    sanitize_engine_update(u);
    last_engine_in_ = u;
    have_engine_in_ = true;
    if (engine_) engine_->set(last_engine_in_);
  }

  const float* buses[kBuses] = {};
  float* sfx_buses[kBuses] = {};

  if (engine_) {
    engine_->render(bus_bufs_[int(Bus::kEngine)].data(), frames);
    buses[int(Bus::kEngine)] = bus_bufs_[int(Bus::kEngine)].data();
  } else if (sfx_) {
    std::fill(bus_bufs_[int(Bus::kEngine)].begin(), bus_bufs_[int(Bus::kEngine)].begin() + ptrdiff_t(samples), 0.0f);
    buses[int(Bus::kEngine)] = bus_bufs_[int(Bus::kEngine)].data();
  }

  drain_ui_music_loaded();
  // The own "Title" snapshot and the title bus mute are a fallback for
  // installs without AudioMixerSnapshots.xml. The game's matrix has no
  // Title snapshot, so with the matrix loaded the title mixes only through
  // the matrix's Gameplay state (AudioHost maps it to UIExterior).
  const bool on_title = !use_matrix_ && snap_title_ >= 0 && mixer_.snapshots().active(snap_title_);
  const bool user_music_on = user_music_ && user_music_->active();
  const bool runtime_radio_on = !user_music_on && runtime_radio_ && runtime_radio_->active();

  // Homespace interior radio stations (Radio6_3d_Autoshow / Radio8_3d_Paintshop / Radio7_3d_Workshop).
  const int eff_homespace = homespace_mode_ > 0 ? homespace_mode_ : int(last_veh_state_.hub_ambience);
  bool homespace_radio_active = false;
  if (!on_title && !user_music_on && ui_music_mode_ == UiMusicMode::kHomespace && eff_homespace >= 1 &&
      eff_homespace <= 3 && radio_player_) {
    auto& hbuf = bus_bufs_[int(Bus::kHomespaceMusic)];
    std::fill(hbuf.begin(), hbuf.begin() + ptrdiff_t(samples), 0.0f);
    if (radio_player_->render_homespace_station(eff_homespace, hbuf.data(), frames)) {
      buses[int(Bus::kHomespaceMusic)] = hbuf.data();
      homespace_radio_active = true;
    }
  } else if (radio_player_) {
    radio_player_->stop_homespace_voices();
  }

  const bool have_want_ui_track =
      (!user_music_on && !homespace_radio_active && ui_music_mode_ != UiMusicMode::kNone &&
       size_t(ui_music_mode_) < ui_tracks_.size() && ui_tracks_[size_t(ui_music_mode_)] &&
       !ui_tracks_[size_t(ui_music_mode_)]->stereo.empty());
  const UiMusicMode want_ui_mode = have_want_ui_track ? ui_music_mode_ : UiMusicMode::kNone;

  // Smooth 250 ms crossfade envelope on UI music fade-in and fade-out (zero hard-cut clicks).
  if (ui_music_cur_ == UiMusicMode::kNone && want_ui_mode != UiMusicMode::kNone) {
    ui_music_cur_ = want_ui_mode;
    ui_music_phase_ = 0.0;
    ui_music_gain_ = 0.0f;
  }
  if (ui_music_cur_ != UiMusicMode::kNone) {
    const StereoTrack* cur_track =
        size_t(ui_music_cur_) < ui_tracks_.size() ? ui_tracks_[size_t(ui_music_cur_)] : nullptr;
    const bool fading_out = (ui_music_cur_ != want_ui_mode);
    if (cur_track && !cur_track->stereo.empty() && (!fading_out || ui_music_gain_ > 1e-4f)) {
      const Bus ui_music_bus = (ui_music_cur_ == UiMusicMode::kHomespace) ? Bus::kHomespaceMusic : Bus::kMenuMusic;
      auto& mbuf = bus_bufs_[int(ui_music_bus)];
      if (!buses[int(ui_music_bus)]) {
        std::fill(mbuf.begin(), mbuf.begin() + ptrdiff_t(samples), 0.0f);
        buses[int(ui_music_bus)] = mbuf.data();
      }
      const size_t total_f = cur_track->stereo.size() / 2;
      const double step = double(std::max(1, cur_track->rate)) / double(rate_);
      const float ramp = 1.0f / (0.25f * float(rate_));
      const float base_gain = (ui_music_cur_ == UiMusicMode::kMap) ? 0.50f : 0.72f;
      for (int i = 0; i < frames; ++i) {
        ui_music_gain_ = fading_out ? std::max(0.0f, ui_music_gain_ - ramp) : std::min(1.0f, ui_music_gain_ + ramp);
        const size_t k = size_t(ui_music_phase_) % total_f;
        const size_t k1 = (k + 1) % total_f;
        const float frac = float(ui_music_phase_ - std::floor(ui_music_phase_));
        const float gl = cur_track->stereo[2 * k] + (cur_track->stereo[2 * k1] - cur_track->stereo[2 * k]) * frac;
        const float gr = cur_track->stereo[2 * k + 1] + (cur_track->stereo[2 * k1 + 1] - cur_track->stereo[2 * k + 1]) * frac;
        const float g = base_gain * ui_music_gain_;
        mbuf[2 * size_t(i)] += gl * g;
        mbuf[2 * size_t(i) + 1] += gr * g;
        ui_music_phase_ += step;
        if (ui_music_phase_ >= double(total_f)) ui_music_phase_ = std::fmod(ui_music_phase_, double(total_f));
      }
      if (fading_out && ui_music_gain_ <= 1e-4f) {
        ui_music_gain_ = 0.0f;
        ui_music_phase_ = 0.0;
        ui_music_cur_ = want_ui_mode;
      }
    } else {
      ui_music_gain_ = 0.0f;
      ui_music_phase_ = 0.0;
      ui_music_cur_ = want_ui_mode;
    }
  }

  const bool ui_music_active = homespace_radio_active || (want_ui_mode != UiMusicMode::kNone);
  if (!ui_music_active && !on_title && (user_music_on || runtime_radio_on || radio_player_)) {
    auto& mbuf = bus_bufs_[int(Bus::kMusic)];
    std::fill(mbuf.begin(), mbuf.begin() + ptrdiff_t(samples), 0.0f);
    if (user_music_on) {
      // Your Music mutes the game's radio while keeping all other buses.
      user_music_->render(mbuf.data(), frames);
    } else if (runtime_radio_on) {
      runtime_radio_->render(mbuf.data(), frames);
    } else if (radio_player_) {
      radio_player_->set_dj_modifier(use_matrix_ ? double(matrix_.gain(Category::kDJModifier)) : 1.0);
      radio_player_->render(mbuf.data(), frames);
    }
    // DynamicsData MusicHighpass (SPEC-AUDIO-EVENTS Part 3): smooth 1-pole HPF
    // on Bus::kMusic while AverageSpeedEnter is active in SnapshotGroup::kSpeedZones.
    // fadeinrate / fadeoutrate are linear mix slew rates per second (3.0 / 33.0 s^-1).
    if (use_matrix_) {
      const auto& dyn = matrix_.dynamics();
      const bool hp_active = matrix_.active_name(SnapshotGroup::kSpeedZones) == "AverageSpeedEnter";
      const float target_mix = hp_active ? 1.0f : 0.0f;
      if (target_mix > 1e-4f || music_hp_mix_ > 1e-4f) {
        const float rate_per_s = std::max(0.05f, hp_active ? dyn.music_hp_fadein : dyn.music_hp_fadeout);
        const float mix_step = rate_per_s / float(rate_);
        const float fc = std::clamp(dyn.music_hp_cutoff, 20.0f, 4000.0f);
        const float rc = 1.0f / (2.0f * 3.14159265f * fc);
        const float alpha = rc / (rc + 1.0f / float(rate_));
        for (int i = 0; i < frames; ++i) {
          music_hp_mix_ = target_mix > music_hp_mix_ ? std::min(target_mix, music_hp_mix_ + mix_step)
                                                     : std::max(target_mix, music_hp_mix_ - mix_step);
          for (int ch = 0; ch < 2; ++ch) {
            const float x = safe_f(mbuf[2 * size_t(i) + size_t(ch)], 0.0f);
            float y = alpha * (music_hp_prev_out_[ch] + x - music_hp_prev_in_[ch]);
            if (!std::isfinite(y) || std::fabs(y) < 1e-20f) y = 0.0f;
            music_hp_prev_in_[ch] = x;
            music_hp_prev_out_[ch] = y;
            mbuf[2 * size_t(i) + size_t(ch)] = x + (y - x) * music_hp_mix_;
          }
        }
      }
    }
    // Radio Rally Mode: smoothly attenuate and high-pass filter Bus::kMusic
    // during Rally stages (and duck further while Co-Driver speaks), keeping
    // Bus::kRallyCoDriver clear while preserving custom radio / User Music.
    {
      const bool cd_speaking = codriver_ && codriver_->playing();
      const float target_g = !radio_rally_mode_ ? 1.0f
                             : use_matrix_      ? (cd_speaking ? 0.50f : 1.0f)
                                                : (cd_speaking ? 0.15f : 0.30f);
      const float target_hp = radio_rally_mode_ ? 1.0f : 0.0f;
      if (radio_rally_mode_ || std::fabs(rally_radio_gain_ - 1.0f) > 1e-4f || rally_hp_mix_ > 1e-4f) {
        const float sm = 1.0f - std::exp(-1.0f / (0.04f * float(rate_)));
        const float fc = 260.0f;
        const float rc = 1.0f / (2.0f * 3.14159265f * fc);
        const float alpha = rc / (rc + 1.0f / float(rate_));
        for (int i = 0; i < frames; ++i) {
          rally_radio_gain_ += (target_g - rally_radio_gain_) * sm;
          rally_hp_mix_ += (target_hp - rally_hp_mix_) * sm;
          for (int ch = 0; ch < 2; ++ch) {
            const float x = safe_f(mbuf[2 * size_t(i) + size_t(ch)], 0.0f);
            float y = alpha * (rally_hp_prev_out_[ch] + x - rally_hp_prev_in_[ch]);
            if (!std::isfinite(y) || std::fabs(y) < 1e-20f) y = 0.0f;
            rally_hp_prev_in_[ch] = x;
            rally_hp_prev_out_[ch] = y;
            mbuf[2 * size_t(i) + size_t(ch)] = (x + (y - x) * rally_hp_mix_) * rally_radio_gain_;
          }
        }
      }
    }
    buses[int(Bus::kMusic)] = mbuf.data();
  }

  if (sfx_) {
    constexpr Bus kSfxBuses[] = {
        Bus::kOtherCars,        Bus::kTyres,         Bus::kWind,         Bus::kAmbience, Bus::kUi,
        Bus::kTransmission,     Bus::kSupercharger,  Bus::kTurbo,        Bus::kEngineDamage, Bus::kBackfire,
        Bus::kSuspension,       Bus::kCollision,     Bus::kHomespaceAmbience,
    };
    for (Bus b : kSfxBuses) {
      auto& buf = bus_bufs_[int(b)];
      std::fill(buf.begin(), buf.begin() + ptrdiff_t(samples), 0.0f);
      sfx_buses[int(b)] = buf.data();
      buses[int(b)] = buf.data();
    }
    sfx_buses[int(Bus::kEngine)] = bus_bufs_[int(Bus::kEngine)].data();
    last_veh_state_.music_active = buses[int(Bus::kMusic)] != nullptr || buses[int(Bus::kHomespaceMusic)] != nullptr ||
                                   buses[int(Bus::kMenuMusic)] != nullptr;
    sfx_->render(last_veh_state_, sfx_buses, frames, engine_, lfe_buf_.data());
  }

  bool satnav_now = false;
  bool vo_now = false;
  if ((satnav_ || vo_ || codriver_) && !on_title) {
    if (satnav_) {
      auto& sbuf = bus_bufs_[int(Bus::kSatNav)];
      std::fill(sbuf.begin(), sbuf.begin() + ptrdiff_t(samples), 0.0f);
      satnav_now = satnav_->render(sbuf.data(), frames);
      if (satnav_now) buses[int(Bus::kSatNav)] = sbuf.data();
    }
    if (vo_) {
      auto& vbuf = bus_bufs_[int(Bus::kVo)];
      std::fill(vbuf.begin(), vbuf.begin() + ptrdiff_t(samples), 0.0f);
      vo_now = vo_->render(vbuf.data(), frames);
      if (vo_now) buses[int(Bus::kVo)] = vbuf.data();
    }
    if (codriver_) {
      auto& cbuf = bus_bufs_[int(Bus::kRallyCoDriver)];
      std::fill(cbuf.begin(), cbuf.begin() + ptrdiff_t(samples), 0.0f);
      if (codriver_->render(cbuf.data(), frames)) {
        // Only fall back to VOPlaying when the v4 22nd RallyCoDriver category is absent.
        if (!use_matrix_ || matrix_.category_count() < 22) vo_now = true;
        buses[int(Bus::kRallyCoDriver)] = cbuf.data();
      }
    }
  }

  // World ambience bed (soundscape regional & festival crowd beds) + 3D location radio stations.
  if (!on_title && (have_acoustics_ || radio_player_)) {
    auto& abuf = bus_bufs_[int(Bus::kAmbience)];
    if (!buses[int(Bus::kAmbience)]) {
      std::fill(abuf.begin(), abuf.begin() + ptrdiff_t(samples), 0.0f);
      buses[int(Bus::kAmbience)] = abuf.data();
    }
    if (have_acoustics_) {
      AmbienceParams& ap = ambience_params_;
      float h = std::fmod(tod_hour_, 24.0f);
      if (!(h >= 0.0f)) h = std::isfinite(h) ? h + 24.0f : 12.0f;
      ap.current_time_s = h * 3600.0f;
      ap.car_speed_mph = ap.speed = std::isfinite(last_veh_state_.speed_mph) ? std::fabs(last_veh_state_.speed_mph) : 0.0f;
      ap.engine_rpm = std::isfinite(last_veh_state_.rpm) ? last_veh_state_.rpm : 0.0f;
      ap.radio_is_playing = (user_music_on || runtime_radio_on || (radio_ && radio_->on())) ? 1.0f : 0.0f;
      ambience_bed_.render(last_acoustics_, ap, abuf.data(), frames);
    }
    if (radio_player_) {
      if (!user_music_on) {
        radio_player_->render_location_stations(abuf.data(), frames);
      } else if (radio_) {
        radio_->advance_location_clock(double(frames) / double(rate_));
      }
    }
  }

  // Apply GlobalDSPConfig.xml car-bus EQ to active car buses (independent filter state per bus).
  constexpr Bus kCarBuses[9] = {Bus::kEngine,       Bus::kOtherCars, Bus::kTyres,
                                Bus::kWind,         Bus::kTransmission, Bus::kSupercharger,
                                Bus::kTurbo,        Bus::kEngineDamage, Bus::kBackfire};
  for (int i = 0; i < 9; ++i) {
    if (buses[int(kCarBuses[i])]) {
      car_eqs_[size_t(i)].process(bus_bufs_[int(kCarBuses[i])].data(), frames);
    }
  }

  // Per-bus post-fader environmental reverb send weights (honoring
  // AudioEngineConfig.xml backfireReverbWetSendOverride instead of hardcoded 1.0).
  const float bf_rev_send = rms_stage_.config().backfire_reverb_wet_send_linear();
  auto rev_send_weight = [bf_rev_send](Bus b) -> float {
    switch (b) {
      case Bus::kEngine: return 1.0f;
      case Bus::kTransmission: return 0.85f;
      case Bus::kSupercharger: return 0.85f;
      case Bus::kTurbo: return 0.85f;
      case Bus::kBackfire: return bf_rev_send;
      case Bus::kOtherCars: return 0.85f;
      case Bus::kTyres: return 0.65f;
      case Bus::kCollision: return 0.90f;
      case Bus::kSuspension: return 0.70f;
      default: return 0.0f;
    }
  };
  if (have_acoustics_) {
    if (reverb_send_buf_.size() < samples) {
      reverb_send_buf_.resize(samples, 0.0f);
      reverb_wet_buf_.resize(samples, 0.0f);
    }
    std::fill_n(reverb_send_buf_.data(), samples, 0.0f);
  }

  // Unified VO ducking state: coherent union of active VoPlayer playback and UI dialog ducking.
  const bool vo_duck_now = vo_now || dialog_duck_requested_;

  if (use_matrix_) {
    const bool dj_now = !on_title && !user_music_on && !runtime_radio_on && radio_player_ && radio_player_->dj_speaking();
    const bool fest_now =
        !on_title && !user_music_on && !runtime_radio_on && radio_player_ && radio_player_->festival_update_playing();
    if (fest_now != last_festival_update_ || dj_now != last_dj_speaking_) {
      last_festival_update_ = fest_now;
      last_dj_speaking_ = dj_now;
      if (fest_now) matrix_.activate("RadioFestivalUpdate");
      else if (dj_now) matrix_.activate("RadioDJSpeaking");
      else matrix_.activate("RadioNormal");
    }
    if (satnav_now != last_satnav_playing_) {
      last_satnav_playing_ = satnav_now;
      matrix_.activate(satnav_now ? "SatNavPlaying" : "SatNavNotPlaying");
    }
    if (vo_duck_now != last_vo_playing_) {
      last_vo_playing_ = vo_duck_now;
      matrix_.activate(vo_duck_now ? "VOPlaying" : "VONotPlaying");
    }
    if (have_acoustics_) {
      if (last_acoustics_.in_tunnel != last_in_tunnel_) {
        last_in_tunnel_ = last_acoustics_.in_tunnel;
        matrix_.activate(last_in_tunnel_ ? "InTunnel" : "NotInTunnel");
      }
      const std::string_view snap_v = last_acoustics_.ambience_snapshot_view();
      const std::string_view prev_v(last_ambience_snap_.data());
      if (!snap_v.empty() && snap_v != prev_v) {
        const size_t n = std::min(snap_v.size(), last_ambience_snap_.size() - 1);
        std::memcpy(last_ambience_snap_.data(), snap_v.data(), n);
        last_ambience_snap_[n] = '\0';
        matrix_.activate(snap_v);
      }
    }
    std::fill(out, out + samples, 0.0f);
    const float a = 1.0f - std::exp(-1.0f / (0.01f * float(rate_)));
    bool any_rev_sent = false;
    for (int b = 0; b < kBuses; ++b) {
      // AudioMixerSnapshots.xml is the single source of truth in matrix mode;
      // MenuMusic and HomespaceMusic honor snapshot + VO/SatNav ducking directly.
      const float snap_g =
          (on_title && Bus(b) != Bus::kUi && Bus(b) != Bus::kMenuMusic && Bus(b) != Bus::kVo) ? 0.0f
                                                                                             : matrix_.bus_gain(Bus(b));
      const float target = mixer_.bus_level(Bus(b)) * snap_g;
      float& s = matrix_bus_smooth_[size_t(b)];
      const float* in = buses[b];
      if (!in) {
        s = target;
        continue;
      }
      const float rw = have_acoustics_ ? rev_send_weight(Bus(b)) : 0.0f;
      if (rw > 1e-5f) {
        any_rev_sent = true;
        for (int i = 0; i < frames; ++i) {
          s += (target - s) * a;
          const float sl = in[2 * i] * s;
          const float sr = in[2 * i + 1] * s;
          out[2 * i] += sl;
          out[2 * i + 1] += sr;
          reverb_send_buf_[2 * size_t(i)] += sl * rw;
          reverb_send_buf_[2 * size_t(i) + 1] += sr * rw;
        }
      } else {
        for (int i = 0; i < frames; ++i) {
          s += (target - s) * a;
          out[2 * i] += in[2 * i] * s;
          out[2 * i + 1] += in[2 * i + 1] * s;
        }
      }
    }
    // Post-fader / post-snapshot environmental reverb return summed directly into master
    // (dry buses stay untouched: zero cross-bus bleed, zero paused excitation, zero Rewind loss).
    if (have_acoustics_ && any_rev_sent &&
        reverb_.process_wet(reverb_send_buf_.data(), reverb_wet_buf_.data(), frames)) {
      for (size_t s = 0; s < samples; ++s) out[s] += reverb_wet_buf_[s];
    }
    const float m_target = mixer_.master();
    const float mute_target = mixer_.muted() ? 0.0f : 1.0f;
    const float lfe_gate =
        on_title ? 0.0f
                 : std::clamp(std::max(matrix_bus_smooth_[int(Bus::kEngine)], matrix_bus_smooth_[int(Bus::kCollision)]),
                              0.0f, 2.0f);
    for (int i = 0; i < frames; ++i) {
      matrix_master_smooth_ += (m_target - matrix_master_smooth_) * a;
      matrix_mute_smooth_ += (mute_target - matrix_mute_smooth_) * a;
      const float g = matrix_master_smooth_ * matrix_mute_smooth_;
      out[2 * i] *= g;
      out[2 * i + 1] *= g;
      lfe_buf_[size_t(i)] *= g * lfe_gate;
    }
    // RMS compressor processes stereo L/R without LFE, then folds uncompressed LFE
    // into L/R immediately before the single master_saturate() stage.
    rms_stage_.process(out, frames, 2, sfx_ ? lfe_buf_.data() : nullptr);
    const double dt = std::min(double(frames) / double(rate_), 0.05);
    matrix_.update_dynamics(dt, last_veh_state_.speed_mph, last_veh_state_.top_speed_mph);
    matrix_.advance(dt);
    mixer_.snapshots().advance(dt);
  } else {
    if (vo_duck_now != last_vo_playing_) {
      last_vo_playing_ = vo_duck_now;
      if (snap_dialog_duck_ >= 0) mixer_.snapshots().set_active(snap_dialog_duck_, vo_duck_now);
    }
    bool have_wet = false;
    if (have_acoustics_) {
      bool any_rev_sent = false;
      for (int b = 0; b < kBuses; ++b) {
        const float* src = buses[b];
        if (!src) continue;
        const float rw = rev_send_weight(Bus(b));
        if (rw <= 1e-5f) continue;
        const float eff_g = mixer_.bus_level(Bus(b)) * mixer_.snapshots().level(Bus(b));
        if (eff_g <= 1e-5f) continue;
        any_rev_sent = true;
        const float k = eff_g * rw;
        for (size_t s = 0; s < samples; ++s) reverb_send_buf_[s] += src[s] * k;
      }
      if (any_rev_sent) {
        have_wet = reverb_.process_wet(reverb_send_buf_.data(), reverb_wet_buf_.data(), frames);
      }
    }
    const float lfe_gate =
        std::clamp(std::max(mixer_.bus_level(Bus::kEngine) * mixer_.snapshots().level(Bus::kEngine),
                            mixer_.bus_level(Bus::kCollision) * mixer_.snapshots().level(Bus::kCollision)),
                   0.0f, 2.0f);
    mixer_.mix(buses, frames, out);
    const float master_g = mixer_.muted() ? 0.0f : mixer_.master();
    if (have_wet) {
      for (size_t s = 0; s < samples; ++s) out[s] += reverb_wet_buf_[s] * master_g;
    }
    if (sfx_) {
      const float lk = master_g * lfe_gate * 0.70710678f;
      if (lk > 1e-5f) {
        for (int i = 0; i < frames; ++i) {
          const float v = lfe_buf_[size_t(i)] * lk;
          out[2 * i] += v;
          out[2 * i + 1] += v;
        }
      }
    }
  }
  sanitize_output(out, frames);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double budget_ms = 1000.0 * double(frames) / double(rate_);
  publish_telemetry(ms, budget_ms);
}

void AudioEngine::publish_telemetry(double block_ms, double budget_ms) {
  ++blocks_rendered_;
  total_block_ms_ += block_ms;
  worst_block_ms_ = std::max(worst_block_ms_, block_ms);
  if (block_ms > budget_ms) ++stalls_;
  recent_block_ms_[recent_block_pos_] = block_ms;
  recent_block_pos_ = (recent_block_pos_ + 1) % recent_block_ms_.size();
  if (recent_block_count_ < recent_block_ms_.size()) ++recent_block_count_;
  double p95_ms = block_ms;
  if (recent_block_count_ > 0) {
    std::array<double, 256> sorted{};
    std::copy_n(recent_block_ms_.begin(), recent_block_count_, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + ptrdiff_t(recent_block_count_));
    const size_t idx = std::min(recent_block_count_ - 1, size_t(0.95 * double(recent_block_count_ - 1) + 0.5));
    p95_ms = sorted[idx];
  }

  AudioTelemetry t{};
  if (radio_) {
    t.radio_popup = radio_->popup();
    t.radio_selected = radio_->selected();
    t.radio_clock = radio_->clock();
    t.radio_dj = radio_->dj_enabled();
  }
  if (user_music_) {
    t.user_music_active = user_music_->active();
    const std::string_view title = user_music_->current_title_view();
    const size_t n = std::min(title.size(), t.user_music_title.size() - 1);
    if (n > 0) std::memcpy(t.user_music_title.data(), title.data(), n);
    t.user_music_title[n] = '\0';
  }
  if (runtime_radio_) {
    t.runtime_radio_active = runtime_radio_->active();
    t.runtime_radio_station = runtime_radio_->selected_station();
    t.runtime_radio_track_seq = runtime_radio_->track_seq();
    const std::string_view title = runtime_radio_->current_title_view();
    const size_t nt = std::min(title.size(), t.runtime_radio_title.size() - 1);
    if (nt > 0) std::memcpy(t.runtime_radio_title.data(), title.data(), nt);
    t.runtime_radio_title[nt] = '\0';
    const std::string_view artist = runtime_radio_->current_artist_view();
    const size_t na = std::min(artist.size(), t.runtime_radio_artist.size() - 1);
    if (na > 0) std::memcpy(t.runtime_radio_artist.data(), artist.data(), na);
    t.runtime_radio_artist[na] = '\0';
  }
  t.peak = use_matrix_ ? rms_stage_.peak() : mixer_.stage().peak();
  t.gain_reduction_db = use_matrix_ ? rms_stage_.gain_reduction_db() : mixer_.stage().gain_reduction_db();
  t.blocks_rendered = blocks_rendered_;
  t.avg_block_ms = blocks_rendered_ > 0 ? (total_block_ms_ / double(blocks_rendered_)) : 0.0;
  t.p95_block_ms = p95_ms;
  t.worst_block_ms = worst_block_ms_;
  t.stalls = stalls_;
  t.underruns = underruns_;
  t.backfire_count = sfx_ ? sfx_->backfire_count() : 0u;
  t.codriver_completed_cues = codriver_ ? codriver_->completed_cue_count() : 0u;
  t.satnav_playing = satnav_ && satnav_->playing();
  t.vo_playing = (vo_ && vo_->playing()) || (codriver_ && codriver_->playing());
  if (vo_) {
    t.vo_elapsed_s = vo_->elapsed_s();
    t.vo_duration_s = vo_->duration_s();
    t.vo_rms = vo_->current_rms();
    t.vo_active_trigger_hash = vo_->active_trigger_hash();
    const std::string& trig = vo_->current_trigger();
    const size_t nt = std::min(trig.size(), t.vo_active_trigger.size() - 1);
    if (nt > 0) std::memcpy(t.vo_active_trigger.data(), trig.data(), nt);
    t.vo_active_trigger[nt] = '\0';
  }
  t.in_tunnel = have_acoustics_ && last_acoustics_.in_tunnel;
  t.radio_rally_mode = radio_rally_mode_;
  telemetry_buf_.write(t);
}

void AudioEngine::render_offline(float* out, int frames) {
  int done = 0;
  while (done < frames) {
    const int n = std::min(frames - done, 4096);
    render_block(out + size_t(done) * 2, n);
    done += n;
  }
}

void AudioEngine::thread_main() {
  enable_audio_thread_rt_mode();
#ifdef _WIN32
  // High-resolution waitable timer avoids Windows' default 15.6 ms sleep granularity,
  // which otherwise lets the audio thread oversleep past a 40-60 ms stream buffer.
  HANDLE hr_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  auto sleep_ms = [hr_timer](int ms) {
    if (hr_timer) {
      LARGE_INTEGER due{};
      due.QuadPart = -int64_t(ms) * 10000LL;
      if (SetWaitableTimer(hr_timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(hr_timer, INFINITE);
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
#else
  auto sleep_ms = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
#endif

  bool stream_started = false;
  const int max_adaptive_extra = std::max(sink_.block_frames * 2, rate_ / 10);  // up to +100 ms headroom
  const int fade_samples = std::max(64, int(0.006 * double(rate_)));            // 6 ms click-free recovery fade

  while (!stop_.load(std::memory_order_acquire)) {
    int queued = sink_.queued_frames ? sink_.queued_frames() : 0;
    if (queued < 0) {
      // Stream error or device disconnect: back off cleanly rather than busy-spinning.
      sleep_ms(5);
      continue;
    }

    if (stream_started && queued == 0) {
      // Hardware buffer drained during a severe CPU/OS stall: smoothly fade in the
      // recovered audio and temporarily widen buffer headroom to break any
      // post-stutter underrun oscillation loop.
      ++underruns_;
      recovery_fade_pos_ = 0;
      recovery_fade_len_ = fade_samples;
      adaptive_extra_frames_ = std::min(max_adaptive_extra, adaptive_extra_frames_ + rate_ / 25);
      clean_blocks_since_underrun_ = 0;
    }

    const int target = sink_.target_buffer_frames + adaptive_extra_frames_;
    // Bound catch-up per wake-up so a stalled sink clock can never trigger an unbounded burst.
    const int max_blocks_per_wake = std::max(2, (target + sink_.block_frames - 1) / sink_.block_frames + 2);
    int blocks_written = 0;
    bool did_work = false;
    while (!stop_.load(std::memory_order_acquire) && queued < target && blocks_written < max_blocks_per_wake) {
      const int n = sink_.block_frames;
      render_block(out_buf_.data(), n);
      if (sink_.write) sink_.write(out_buf_.data(), n);
      stream_started = true;
      ++blocks_written;
      did_work = true;
      if (adaptive_extra_frames_ > 0) {
        if (++clean_blocks_since_underrun_ >= 300) {
          adaptive_extra_frames_ = std::max(0, adaptive_extra_frames_ - sink_.block_frames);
          clean_blocks_since_underrun_ = 250;
        }
      }
      // Re-query real queued depth after each block: if this render was preempted
      // during a CPU spike, an optimistic `queued += n` would under-fill the buffer
      // and immediately cause another underrun on the next sleep.
      if (sink_.queued_frames) {
        const int now_q = sink_.queued_frames();
        if (now_q < 0) break;
        queued = now_q;
      } else {
        queued += n;
      }
    }
    if (!did_work) sleep_ms(2);
  }
#ifdef _WIN32
  if (hr_timer) CloseHandle(hr_timer);
#endif
}

}  // namespace audio
