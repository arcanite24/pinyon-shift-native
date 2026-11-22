// Dedicated non-blocking audio render thread and mix host (ours): owns the
// Mixer, EnginePlayer, RadioPlayer, and UserMusicPlayer, and replaces
// main-thread audio rendering. All main-thread calls are lock-free: discrete
// changes go through an SPSC command queue, per-frame engine parameters use
// a TripleBuffer, and audio-thread state (radio popup, meters, timing) comes
// back through a TripleBuffer. Swapped players are returned through a retire
// queue so the audio thread never allocates or frees heap memory in steady
// state.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/engine_player.h"
#include "audio/mixer.h"
#include "audio/radio.h"
#include "audio/rt.h"
#include "audio/sfx_engine.h"
#include "audio/snapshots.h"
#include "audio/user_music.h"

namespace audio {

enum class CarDspMode : uint8_t { kOff = 0, kRewind, kCountdown, kCockpit };

struct AudioTelemetry {
  Radio::Popup radio_popup{};
  int radio_selected = -1;
  double radio_clock = 0.0;
  bool radio_dj = true;
  bool user_music_active = false;
  std::array<char, 96> user_music_title{};
  float peak = 0.0f;
  float gain_reduction_db = 0.0f;
  uint64_t blocks_rendered = 0;
  double worst_block_ms = 0.0;
  uint64_t stalls = 0;  // blocks that exceeded their real-time duration
  uint32_t backfire_count = 0;  // monotonic exhaust backfire pop counter (coupled to visual flames)
};

class AudioEngine {
 public:
  struct Sink {
    std::function<int()> queued_frames;
    std::function<void(const float* stereo, int frames)> write;
    int target_buffer_frames = 2400;  // ~50 ms at 48 kHz
    int block_frames = 480;           // ~10 ms at 48 kHz
  };

  explicit AudioEngine(int rate = 48000);
  ~AudioEngine();

  int rate() const { return rate_; }

  // Starts/stops the dedicated render thread.
  void start(Sink sink);
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  // Synchronous offline rendering (for headless --audio-out and tests; call
  // only when the render thread is not running).
  void render_offline(float* out, int frames);

  // Main-thread lock-free parameter & player handoff.
  void set_engine_input(const EnginePlayer::Update& u);
  void set_vehicle_state(const VehicleAudioState& st);
  void set_engine_player(std::unique_ptr<EnginePlayer> player);
  void set_sfx_engine(std::unique_ptr<SfxEngine> sfx);
  // Attaches `radio` (caller keeps `RadioData` and `Radio` alive until
  // AudioEngine is stopped/destroyed or detached with nullptr) and its player.
  void set_radio(Radio* radio, std::unique_ptr<RadioPlayer> player);
  void set_user_music(std::unique_ptr<UserMusicPlayer> player);

  // Lock-free discrete SFX/UI triggers.
  void trigger_collision(const CollisionHit& hit);
  void trigger_ui(UiCue cue);

  // Mixer controls (Sound Mix 0..2 per bus, master volume/mute, compressor).
  void set_bus_level(Bus b, float level);
  void set_master_volume(float volume);
  void set_mute(bool mute);
  void set_full_dynamic_range(bool on);
  // Built-in snapshot ids ("Paused", "Menu", "Title", "Loading"), or any
  // snapshot registered before start().
  int register_snapshot(Snapshot snap);
  int find_snapshot(std::string_view name) const;
  void set_snapshot(int id, bool active);

  // Loads AudioMixerSnapshots.xml, GlobalDSPConfig.xml, and AudioEngineConfig.xml
  // from `game_root` (call before start()). When loaded, the 9x22 SnapshotMatrix,
  // CarBusEq, and MasterRmsStage drive the mix.
  bool load_mixer_configs(const std::filesystem::path& game_root, std::string* note = nullptr);
  bool has_snapshot_matrix() const { return use_matrix_; }
  int find_matrix_snapshot(std::string_view name) const { return matrix_.find(name); }
  void activate_matrix_snapshot(std::string_view name);
  void activate_matrix_snapshot_idx(int idx);
  void restore_gameplay_snapshot();
  void set_dynamics_speed(float speed_mph, float top_speed_mph = 140.0f);
  void set_car_bus_dsp_mode(CarDspMode mode);

  // Radio commands (executed on the audio thread so Radio is single-threaded).
  void radio_press(int dir);
  void radio_select(int station);
  void radio_set_dj(bool on);
  void radio_set_volume(float volume);

  // Your Music commands.
  void user_music_enable(bool on);
  void user_music_set_shuffle(bool on);
  void user_music_set_volume(float volume);
  void user_music_next();
  void user_music_prev();

  // Main-thread lock-free read of the latest audio-thread state.
  AudioTelemetry telemetry();
  // Frees any players retired by the audio thread (also called automatically
  // on main-thread setter calls and stop()).
  void reclaim();

 private:
  struct Command {
    enum Kind : uint8_t {
      kBusLevel,
      kMasterVolume,
      kMute,
      kFullDynamicRange,
      kSnapshot,
      kRadioPress,
      kRadioSelect,
      kRadioDj,
      kRadioVolume,
      kUserMusicEnable,
      kUserMusicShuffle,
      kUserMusicVolume,
      kUserMusicNext,
      kUserMusicPrev,
      kSwapEngine,
      kSwapSfx,
      kSwapRadio,
      kSwapUserMusic,
      kTriggerCollision,
      kTriggerUi,
      kMatrixActivate,
      kMatrixRestoreGameplay,
      kDynamicsSpeed,
      kCarBusDspMode,
    };
    Kind kind = kBusLevel;
    int i0 = 0;
    float f0 = 0.0f;
    float f1 = 0.0f;
    float f2 = 0.0f;
    float f3 = 0.0f;
    bool b0 = false;
    bool b1 = false;
    void* ptr0 = nullptr;
    void* ptr1 = nullptr;
  };
  struct Retired {
    enum Kind : uint8_t { kEngine, kSfx, kRadio, kUserMusic };
    Kind kind = kEngine;
    void* ptr = nullptr;
  };

  void enqueue(const Command& c);
  void drain_commands();
  void retire_ptr(Retired::Kind kind, void* ptr);
  void render_block(float* out, int frames);
  void publish_telemetry(double block_ms, double budget_ms);
  void thread_main();

  int rate_ = 48000;
  Mixer mixer_;
  int snap_title_ = -1;

  bool use_matrix_ = false;
  SnapshotMatrix matrix_;
  GlobalDspConfig global_dsp_{};
  std::array<CarBusEq, 4> car_eqs_;
  MasterRmsStage rms_stage_;
  std::array<float, kBuses> matrix_bus_smooth_{};
  float matrix_master_smooth_ = 1.0f;
  float matrix_mute_smooth_ = 1.0f;
  bool last_dj_speaking_ = false;

  // Owned/used exclusively by the audio thread while running.
  EnginePlayer* engine_ = nullptr;
  SfxEngine* sfx_ = nullptr;
  Radio* radio_ = nullptr;
  RadioPlayer* radio_player_ = nullptr;
  UserMusicPlayer* user_music_ = nullptr;

  EnginePlayer::Update last_engine_in_{};
  bool have_engine_in_ = false;
  VehicleAudioState last_veh_state_{};

  std::array<std::vector<float>, kBuses> bus_bufs_;
  std::vector<float> out_buf_;

  TripleBuffer<EnginePlayer::Update> engine_in_;
  TripleBuffer<VehicleAudioState> veh_state_in_;
  TripleBuffer<AudioTelemetry> telemetry_buf_;
  SpscQueue<Command> cmds_{512};
  SpscQueue<Retired> retired_{64};

  uint64_t blocks_rendered_ = 0;
  double worst_block_ms_ = 0.0;
  uint64_t stalls_ = 0;

  Sink sink_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace audio
