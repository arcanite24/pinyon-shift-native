#include "audio/audio_engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace audio {

AudioEngine::AudioEngine(int rate) : rate_(std::max(8000, rate)) {
  mixer_.prepare(rate_);
  mixer_.snapshots().add_defaults();
  snap_title_ = mixer_.snapshots().find("Title");
  // Pre-allocate stereo bus scratch buffers for up to 4096 frames so the
  // audio thread never allocates in steady state.
  bus_engine_.resize(4096 * 2, 0.0f);
  bus_music_.resize(4096 * 2, 0.0f);
  out_buf_.resize(4096 * 2, 0.0f);
}

AudioEngine::~AudioEngine() {
  stop();
  delete engine_;
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
      case Command::kFullDynamicRange: mixer_.stage().set_full_dynamic_range(c.b0); break;
      case Command::kSnapshot: mixer_.snapshots().set_active(c.i0, c.b0); break;
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
      case Command::kSwapRadio:
        retire_ptr(Retired::kRadio, radio_player_);
        radio_ = static_cast<Radio*>(c.ptr0);
        radio_player_ = static_cast<RadioPlayer*>(c.ptr1);
        break;
      case Command::kSwapUserMusic:
        retire_ptr(Retired::kUserMusic, user_music_);
        user_music_ = static_cast<UserMusicPlayer*>(c.ptr0);
        break;
    }
  }
}

void AudioEngine::render_block(float* out, int frames) {
  if (frames <= 0) return;
  const auto t0 = std::chrono::steady_clock::now();
  drain_commands();

  const size_t samples = size_t(frames) * 2;
  if (bus_engine_.size() < samples) bus_engine_.resize(samples, 0.0f);
  if (bus_music_.size() < samples) bus_music_.resize(samples, 0.0f);

  EnginePlayer::Update u{};
  if (engine_in_.read(u)) {
    last_engine_in_ = u;
    have_engine_in_ = true;
    if (engine_) engine_->set(last_engine_in_);
  }

  const float* buses[kBuses] = {};
  if (engine_) {
    engine_->render(bus_engine_.data(), frames);
    buses[int(Bus::kEngine)] = bus_engine_.data();
  }

  const bool on_title = snap_title_ >= 0 && mixer_.snapshots().active(snap_title_);
  const bool user_music_on = user_music_ && user_music_->active();
  if (!on_title && (user_music_on || radio_player_)) {
    std::fill(bus_music_.begin(), bus_music_.begin() + ptrdiff_t(samples), 0.0f);
    if (user_music_on) {
      // Your Music mutes the game's radio while keeping all other buses.
      user_music_->render(bus_music_.data(), frames);
    } else if (radio_player_) {
      radio_player_->render(bus_music_.data(), frames);
    }
    buses[int(Bus::kMusic)] = bus_music_.data();
  }

  mixer_.mix(buses, frames, out);
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
  t.peak = mixer_.stage().peak();
  t.gain_reduction_db = mixer_.stage().gain_reduction_db();
  t.blocks_rendered = blocks_rendered_;
  t.worst_block_ms = worst_block_ms_;
  t.stalls = stalls_;
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
