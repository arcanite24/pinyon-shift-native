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

#include "audio/codriver_vo.h"
#include "audio/engine_player.h"
#include "audio/mixer.h"
#include "audio/radio.h"
#include "audio/rt.h"
#include "audio/runtime_radio.h"
#include "audio/satnav_vo.h"
#include "audio/sfx_engine.h"
#include "audio/snapshots.h"
#include "audio/user_music.h"
#include "audio/vo_player.h"
#include "audio/world_acoustics.h"

namespace audio {

enum class CarDspMode : uint8_t { kOff = 0, kRewind, kCountdown, kCockpit };

struct AudioTelemetry {
  Radio::Popup radio_popup{};
  int radio_selected = -1;
  double radio_clock = 0.0;
  bool radio_dj = true;
  bool user_music_active = false;
  std::array<char, 96> user_music_title{};
  bool runtime_radio_active = false;
  int runtime_radio_station = -1;
  uint32_t runtime_radio_track_seq = 0;
  std::array<char, 96> runtime_radio_title{};
  std::array<char, 96> runtime_radio_artist{};
  float peak = 0.0f;
  float gain_reduction_db = 0.0f;
  uint64_t blocks_rendered = 0;
  double avg_block_ms = 0.0;
  double p95_block_ms = 0.0;
  double worst_block_ms = 0.0;
  uint64_t stalls = 0;     // blocks that exceeded their real-time duration
  uint64_t underruns = 0;  // stream buffer underruns detected and smoothly recovered
  uint32_t backfire_count = 0;  // monotonic exhaust backfire pop counter (coupled to visual flames)
  bool satnav_playing = false;
  bool vo_playing = false;
  bool in_tunnel = false;
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
  void set_satnav_vo(std::unique_ptr<SatNavVoPlayer> satnav);
  void set_codriver_vo(std::unique_ptr<CoDriverVoPlayer> codriver);
  void set_vo_player(std::unique_ptr<VoPlayer> vo);
  void set_acoustics_sample(const AcousticsSample& sample);
  void set_listener_pose(std::array<float, 3> pos, float heading_rad);
  int load_ambience_beds(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir);
  void set_ambience_bed_sample(AmbienceBedSlot slot, PcmSample pcm);
  void reload_sfx_burble(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                         const std::string& burble_bank);
  // Attaches `radio` (caller keeps `RadioData` and `Radio` alive until
  // AudioEngine is stopped/destroyed or detached with nullptr) and its player.
  void set_radio(Radio* radio, std::unique_ptr<RadioPlayer> player);
  void set_runtime_radio(std::unique_ptr<RuntimeRadioPlayer> player);
  void set_user_music(std::unique_ptr<UserMusicPlayer> player);

  // Lock-free FMV stereo PCM callback (called on audio thread when non-null).
  using FmvAudioFn = int (*)(void* ctx, float* out_stereo, int frames);
  void set_fmv_audio(FmvAudioFn fn, void* ctx);

  // Lock-free discrete SFX/UI/VO triggers.
  void trigger_collision(const CollisionHit& hit);
  void trigger_ui(UiCue cue);
  void trigger_satnav(SatNavCue cue);
  void set_satnav_enabled(bool on);
  void trigger_vo(std::string_view trigger_id, bool interrupt = false);
  void skip_vo();
  void trigger_codriver(std::string_view cue_path);
  void clear_codriver();

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
  bool load_mixer_configs(const std::filesystem::path& game_root, std::string* note = nullptr,
                          const std::filesystem::path& state_root = {});
  bool load_snapshot_matrix_xml(std::string_view xml, std::string* error = nullptr);
  bool has_snapshot_matrix() const { return use_matrix_; }
  float matrix_bus_gain(Bus b) const { return matrix_.bus_gain(b); }
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
  void radio_set_time_of_day(double hour);
  void radio_trigger_special(std::string_view gameplay_event);
  void radio_trigger_immediate(std::string_view gameplay_event);
  void radio_trigger_festival_update(std::string_view gameplay_event);

  // Runtime custom radio commands.
  void runtime_radio_select(int station);
  void runtime_radio_set_volume(float volume);
  void runtime_radio_next();
  void runtime_radio_prev();

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
      kRadioTod,
      kRadioSpecial,
      kRadioImmediate,
      kRadioFestivalUpdate,
      kRuntimeRadioSelect,
      kRuntimeRadioVolume,
      kRuntimeRadioNext,
      kRuntimeRadioPrev,
      kUserMusicEnable,
      kUserMusicShuffle,
      kUserMusicVolume,
      kUserMusicNext,
      kUserMusicPrev,
      kSwapEngine,
      kSwapSfx,
      kSwapSatNav,
      kSwapCoDriver,
      kSwapVo,
      kSwapRadio,
      kSwapRuntimeRadio,
      kSwapUserMusic,
      kTriggerCollision,
      kTriggerUi,
      kTriggerSatNav,
      kSatNavEnable,
      kTriggerCoDriver,
      kClearCoDriver,
      kTriggerVo,
      kSkipVo,
      kListenerPose,
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
    std::array<char, 64> text{};
  };
  struct Retired {
    enum Kind : uint8_t { kEngine, kSfx, kSatNav, kCoDriver, kVo, kRadio, kRuntimeRadio, kUserMusic };
    Kind kind = kEngine;
    void* ptr = nullptr;
  };

  void enqueue(const Command& c);
  void drain_commands();
  void retire_ptr(Retired::Kind kind, void* ptr);
  void sanitize_output(float* out, int frames);
  void render_block(float* out, int frames);
  void publish_telemetry(double block_ms, double budget_ms);
  void thread_main();

  int rate_ = 48000;
  Mixer mixer_;
  int snap_title_ = -1;

  bool use_matrix_ = false;
  SnapshotMatrix matrix_;
  GlobalDspConfig global_dsp_{};
  std::array<CarBusEq, 9> car_eqs_;
  MasterRmsStage rms_stage_;
  std::array<float, kBuses> matrix_bus_smooth_{};
  float matrix_master_smooth_ = 1.0f;
  float matrix_mute_smooth_ = 1.0f;
  bool last_dj_speaking_ = false;
  bool last_festival_update_ = false;
  bool last_satnav_playing_ = false;
  bool last_vo_playing_ = false;
  bool last_in_tunnel_ = false;
  std::string last_ambience_snap_;
  bool satnav_enabled_ = true;

  // Owned/used exclusively by the audio thread while running.
  EnginePlayer* engine_ = nullptr;
  SfxEngine* sfx_ = nullptr;
  SatNavVoPlayer* satnav_ = nullptr;
  CoDriverVoPlayer* codriver_ = nullptr;
  VoPlayer* vo_ = nullptr;
  Radio* radio_ = nullptr;
  RadioPlayer* radio_player_ = nullptr;
  RuntimeRadioPlayer* runtime_radio_ = nullptr;
  UserMusicPlayer* user_music_ = nullptr;
  EnvironmentalReverb reverb_;
  AmbienceBedPlayer ambience_bed_{48000};
  AcousticsSample last_acoustics_{};
  bool have_acoustics_ = false;

  EnginePlayer::Update last_engine_in_{};
  bool have_engine_in_ = false;
  VehicleAudioState last_veh_state_{};

  std::array<std::vector<float>, kBuses> bus_bufs_;
  std::vector<float> reverb_send_buf_;
  std::vector<float> reverb_wet_buf_;
  std::vector<float> out_buf_;
  float music_hp_mix_ = 0.0f;
  float music_hp_prev_in_[2]{0.0f, 0.0f};
  float music_hp_prev_out_[2]{0.0f, 0.0f};

  TripleBuffer<EnginePlayer::Update> engine_in_;
  TripleBuffer<VehicleAudioState> veh_state_in_;
  TripleBuffer<AcousticsSample> acoustics_in_;
  TripleBuffer<AudioTelemetry> telemetry_buf_;
  SpscQueue<Command> cmds_{512};
  SpscQueue<Retired> retired_{64};

  uint64_t blocks_rendered_ = 0;
  double total_block_ms_ = 0.0;
  double worst_block_ms_ = 0.0;
  uint64_t stalls_ = 0;
  uint64_t underruns_ = 0;
  int recovery_fade_pos_ = 0;
  int recovery_fade_len_ = 0;
  int adaptive_extra_frames_ = 0;
  int clean_blocks_since_underrun_ = 0;
  std::array<double, 256> recent_block_ms_{};
  size_t recent_block_pos_ = 0;
  size_t recent_block_count_ = 0;

  Sink sink_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<FmvAudioFn> fmv_fn_{nullptr};
  std::atomic<void*> fmv_ctx_{nullptr};
  std::thread thread_;
};

}  // namespace audio
