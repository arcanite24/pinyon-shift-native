#include "audio/audio_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace audio {

AudioEngine::AudioEngine(int rate) : rate_(std::max(8000, rate)) {
  mixer_.prepare(rate_);
  mixer_.snapshots().add_defaults();
  snap_title_ = mixer_.snapshots().find("Title");
  for (auto& eq : car_eqs_) eq.prepare(rate_);
  rms_stage_.prepare(rate_);
  matrix_bus_smooth_.fill(1.0f);
  // Pre-allocate stereo bus scratch buffers for up to 4096 frames so the
  // audio thread never allocates in steady state.
  for (auto& b : bus_bufs_) b.resize(4096 * 2, 0.0f);
  out_buf_.resize(4096 * 2, 0.0f);
}

AudioEngine::~AudioEngine() {
  stop();
  delete engine_;
  delete sfx_;
  delete satnav_;
  delete radio_player_;
  delete user_music_;
}

void AudioEngine::start(Sink sink) {
  stop();
  sink_ = std::move(sink);
  sink_.block_frames = std::clamp(sink_.block_frames, 64, 4096);
  sink_.target_buffer_frames = std::max(sink_.block_frames, sink_.target_buffer_frames);
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
  Retired r;
  while (retired_.pop(r)) {
    switch (r.kind) {
      case Retired::kEngine: delete static_cast<EnginePlayer*>(r.ptr); break;
      case Retired::kSfx: delete static_cast<SfxEngine*>(r.ptr); break;
      case Retired::kSatNav: delete static_cast<SatNavVoPlayer*>(r.ptr); break;
      case Retired::kRadio: delete static_cast<RadioPlayer*>(r.ptr); break;
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
  Command c;
  c.kind = Command::kSwapSfx;
  c.ptr0 = sfx.release();
  if (!cmds_.push(c)) {
    if (!running_.load(std::memory_order_acquire)) {
      drain_commands();
      reclaim();
      if (cmds_.push(c)) return;
    }
    delete static_cast<SfxEngine*>(c.ptr0);
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
  enqueue(c);
}

void AudioEngine::trigger_ui(UiCue cue) {
  Command c;
  c.kind = Command::kTriggerUi;
  c.i0 = int(cue);
  enqueue(c);
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

void AudioEngine::set_bus_level(Bus b, float level) {
  Command c;
  c.kind = Command::kBusLevel;
  c.i0 = int(b);
  c.f0 = level;
  enqueue(c);
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

bool AudioEngine::load_mixer_configs(const std::filesystem::path& game_root, std::string* note) {
  if (running_.load(std::memory_order_acquire) || game_root.empty()) return false;
  std::string err;
  const bool have_matrix = matrix_.load(game_root, &err);
  if (have_matrix) {
    use_matrix_ = true;
    for (int b = 0; b < kBuses; ++b) matrix_bus_smooth_[size_t(b)] = matrix_.bus_gain(Bus(b));
  }
  global_dsp_.load(game_root, nullptr);
  MasterDspConfig mdsp;
  if (mdsp.load(game_root, nullptr)) rms_stage_.set_config(mdsp);
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

void AudioEngine::radio_press(int dir) {
  Command c;
  c.kind = Command::kRadioPress;
  c.i0 = dir;
  enqueue(c);
}

void AudioEngine::radio_select(int station) {
  Command c;
  c.kind = Command::kRadioSelect;
  c.i0 = station;
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
      case Retired::kRadio: delete static_cast<RadioPlayer*>(ptr); break;
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
        if (use_matrix_) matrix_.update_dynamics(0.0, c.f0, c.f1);
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
        if (radio_) radio_->select(c.i0);
        break;
      case Command::kRadioDj:
        if (radio_) radio_->set_dj_enabled(c.b0);
        break;
      case Command::kRadioVolume:
        if (radio_player_) radio_player_->volume = double(c.f0);
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
      case Command::kSwapRadio:
        retire_ptr(Retired::kRadio, radio_player_);
        radio_ = static_cast<Radio*>(c.ptr0);
        radio_player_ = static_cast<RadioPlayer*>(c.ptr1);
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
          sfx_->trigger_collision(hit, last_veh_state_);
        }
        break;
      case Command::kTriggerUi:
        if (sfx_) sfx_->trigger_ui(UiCue(c.i0));
        break;
      case Command::kTriggerSatNav:
        if (satnav_ && satnav_enabled_) satnav_->trigger(SatNavCue(c.i0));
        break;
      case Command::kSatNavEnable:
        satnav_enabled_ = c.b0;
        if (satnav_) satnav_->set_enabled(satnav_enabled_);
        break;
    }
  }
}

void AudioEngine::render_block(float* out, int frames) {
  if (frames <= 0) return;
  const auto t0 = std::chrono::steady_clock::now();

  VehicleAudioState vst{};
  if (veh_state_in_.read(vst)) last_veh_state_ = vst;
  drain_commands();

  const size_t samples = size_t(frames) * 2;
  for (auto& b : bus_bufs_) {
    if (b.size() < samples) b.resize(samples, 0.0f);
  }

  EnginePlayer::Update u{};
  if (engine_in_.read(u)) {
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

  const bool on_title = snap_title_ >= 0 && mixer_.snapshots().active(snap_title_);
  const bool user_music_on = user_music_ && user_music_->active();
  if (!on_title && (user_music_on || radio_player_)) {
    auto& mbuf = bus_bufs_[int(Bus::kMusic)];
    std::fill(mbuf.begin(), mbuf.begin() + ptrdiff_t(samples), 0.0f);
    if (user_music_on) {
      // Your Music mutes the game's radio while keeping all other buses.
      user_music_->render(mbuf.data(), frames);
    } else if (radio_player_) {
      radio_player_->render(mbuf.data(), frames);
    }
    buses[int(Bus::kMusic)] = mbuf.data();
  }

  if (sfx_) {
    constexpr Bus kSfxBuses[] = {Bus::kOtherCars, Bus::kTyres, Bus::kWind, Bus::kAmbience, Bus::kUi};
    for (Bus b : kSfxBuses) {
      auto& buf = bus_bufs_[int(b)];
      std::fill(buf.begin(), buf.begin() + ptrdiff_t(samples), 0.0f);
      sfx_buses[int(b)] = buf.data();
      buses[int(b)] = buf.data();
    }
    sfx_buses[int(Bus::kEngine)] = bus_bufs_[int(Bus::kEngine)].data();
    last_veh_state_.music_active = buses[int(Bus::kMusic)] != nullptr;
    sfx_->render(last_veh_state_, sfx_buses, frames, engine_);
  }

  bool satnav_now = false;
  if (satnav_ && !on_title) {
    auto& vbuf = bus_bufs_[int(Bus::kVo)];
    std::fill(vbuf.begin(), vbuf.begin() + ptrdiff_t(samples), 0.0f);
    satnav_now = satnav_->render(vbuf.data(), frames);
    if (satnav_now) buses[int(Bus::kVo)] = vbuf.data();
  }

  // Apply GlobalDSPConfig.xml car-bus EQ to active car buses (independent filter state per bus).
  constexpr Bus kCarBuses[4] = {Bus::kEngine, Bus::kOtherCars, Bus::kTyres, Bus::kWind};
  for (int i = 0; i < 4; ++i) {
    if (buses[int(kCarBuses[i])]) {
      car_eqs_[size_t(i)].process(bus_bufs_[int(kCarBuses[i])].data(), frames);
    }
  }

  if (use_matrix_) {
    const bool dj_now = !on_title && !user_music_on && radio_player_ && radio_player_->dj_speaking();
    if (dj_now != last_dj_speaking_) {
      last_dj_speaking_ = dj_now;
      matrix_.activate(dj_now ? "RadioDJSpeaking" : "RadioNormal");
    }
    if (satnav_now != last_satnav_playing_) {
      last_satnav_playing_ = satnav_now;
      matrix_.activate(satnav_now ? "SatNavPlaying" : "SatNavNotPlaying");
    }
    std::fill(out, out + samples, 0.0f);
    const float a = 1.0f - std::exp(-1.0f / (0.01f * float(rate_)));
    for (int b = 0; b < kBuses; ++b) {
      // Combine AudioMixerSnapshots.xml matrix gain with the built-in Title/Menu
      // guards and user Sound Mix level (0..2).
      const float snap_g = matrix_.bus_gain(Bus(b)) * mixer_.snapshots().level(Bus(b));
      const float target = mixer_.bus_level(Bus(b)) * snap_g;
      float& s = matrix_bus_smooth_[size_t(b)];
      const float* in = buses[b];
      if (!in) {
        s = target;
        continue;
      }
      for (int i = 0; i < frames; ++i) {
        s += (target - s) * a;
        out[2 * i] += in[2 * i] * s;
        out[2 * i + 1] += in[2 * i + 1] * s;
      }
    }
    const float m_target = mixer_.master();
    const float mute_target = mixer_.muted() ? 0.0f : 1.0f;
    for (int i = 0; i < frames; ++i) {
      matrix_master_smooth_ += (m_target - matrix_master_smooth_) * a;
      matrix_mute_smooth_ += (mute_target - matrix_mute_smooth_) * a;
      const float g = matrix_master_smooth_ * matrix_mute_smooth_;
      out[2 * i] *= g;
      out[2 * i + 1] *= g;
    }
    rms_stage_.process(out, frames, 2);
    const double dt = double(frames) / double(rate_);
    matrix_.update_dynamics(dt, last_veh_state_.speed_mph, last_veh_state_.top_speed_mph);
    matrix_.advance(dt);
    mixer_.snapshots().advance(dt);
  } else {
    mixer_.mix(buses, frames, out);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double budget_ms = 1000.0 * double(frames) / double(rate_);
  publish_telemetry(ms, budget_ms);
}

void AudioEngine::publish_telemetry(double block_ms, double budget_ms) {
  ++blocks_rendered_;
  worst_block_ms_ = std::max(worst_block_ms_, block_ms);
  if (block_ms > budget_ms) ++stalls_;

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
  t.peak = use_matrix_ ? rms_stage_.peak() : mixer_.stage().peak();
  t.gain_reduction_db = use_matrix_ ? rms_stage_.gain_reduction_db() : mixer_.stage().gain_reduction_db();
  t.blocks_rendered = blocks_rendered_;
  t.worst_block_ms = worst_block_ms_;
  t.stalls = stalls_;
  t.backfire_count = sfx_ ? sfx_->backfire_count() : 0u;
  t.satnav_playing = satnav_ && satnav_->playing();
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
  while (!stop_.load(std::memory_order_acquire)) {
    int queued = sink_.queued_frames ? sink_.queued_frames() : 0;
    bool did_work = false;
    while (!stop_.load(std::memory_order_acquire) && queued < sink_.target_buffer_frames) {
      const int n = sink_.block_frames;
      render_block(out_buf_.data(), n);
      if (sink_.write) sink_.write(out_buf_.data(), n);
      queued += n;
      did_work = true;
    }
    if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
}

}  // namespace audio
