// Slice 2 .fev/FSB4 SFX runtime and 3D opponent/traffic car engine player
// (SPEC-AUDIO-EVENTS Parts 1-3). Decodes referenced FSB4 XMA2/MPEG subsounds
// asynchronously on a background worker (with on-disk PCM cache) so the audio
// render thread never blocks, locks, or performs disk I/O.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/engine_player.h"
#include "audio/event_configs.h"
#include "audio/fev.h"
#include "audio/mixer.h"
#include "audio/rt.h"

namespace audio {

struct OtherCarAudio {
  bool active = false;
  bool is_traffic = false;
  Vec3f pos{};
  Vec3f vel{};
  float rpm = 1000.0f;
  float speed_mph = 0.0f;
};

struct WheelAudio {
  uint8_t surface_group = 0;  // 0=OnRoad, 1=OffRoad, 2=Grass, 3=Brick, 4=Trackway
  float slip_combined = 0.0f;
  float wheel_rpm = 0.0f;
  float load = 1.0f;
  float bottom_out_force = 0.0f;
};

struct VehicleAudioState {
  static constexpr int kMaxOtherCars = 6;

  float speed_mph = 0.0f;
  float top_speed_mph = 140.0f;
  float rpm = 800.0f;
  float idle_rpm = 800.0f;
  float redline_rpm = 7000.0f;
  float throttle = 0.0f;
  float engine_load = 0.0f;
  int gear = 1;
  bool shifting = false;
  float clutch_pos = 0.0f;
  uint8_t forced_induction = 0;  // 0=NA, 1=Turbo, 2=Supercharger
  float turbo_psi = 0.0f;
  float turbo_norm_rpm = 0.0f;
  bool bov_open = false;
  float supercharger_norm_rpm = 0.0f;
  int trans_level = 0;
  int turbo_level = 0;
  int sc_level = 0;
  int backfire_level = 0;
  std::array<WheelAudio, 4> wheels{};
  Vec3f listener_pos{};
  Vec3f listener_vel{};
  Vec3f listener_right{1.0f, 0.0f, 0.0f};
  bool cockpit_view = false;
  bool in_race = false;
  bool rewinding = false;
  bool music_active = false;
  std::array<OtherCarAudio, kMaxOtherCars> other_cars{};
};

struct CollisionHit {
  float rel_speed_mps = 0.0f;
  Vec3f pos{};
  bool is_car_on_car = false;
  bool is_break = false;
};

enum class UiCue : uint8_t {
  kAccept = 0,
  kCancel,
  kMove,
  kError,
  kMenuOpen,
  kRewindTapeStart,
  kCountdown321,
  kSignSmash,
  kSkillAward,
  kAchievementReward,
};

enum class SfxSoundId : int {
  kWind = 0,
  kTireRollOnRoad,
  kTireSkidOnRoad,
  kTireRollOffRoad,
  kTireSkidOffRoad,
  kTireRollGrass,
  kTireSkidGrass,
  kSurfaceHardToHard,
  kSurfaceDirtToHard,
  kTransWhine,
  kShiftStart,
  kShiftComplete,
  kTurboSpool,
  kTurboBovLo,
  kTurboBovHi,
  kScWhine,
  kSuspensionBottom,
  kImpact3d,
  kImpactLfe,
  kBreakProp,
  kBackfirePop,
  kBackfireLfe,
  kUiAccept,
  kUiCancel,
  kUiMove,
  kUiError,
  kUiMenuOpen,
  kRewindTapeStart,
  kCountdown321,
  kSignSmash,
  kAchievementReward,
  kOtherCarLoop,
  kTrafficCarLoop,
  kCount
};

class SfxEngine {
 public:
  explicit SfxEngine(int out_rate = 48000);
  ~SfxEngine();

  // Parses .fev projects and XML tables from `game_root`, then starts an
  // async background worker decoding referenced FSB4 subsounds into
  // `cache_dir` (never blocking the audio thread).
  bool load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
            const std::string& burble_bank = {}, std::string* note = nullptr);

  // Injects a PCM sample directly (for unit tests without game data).
  void set_sample(SfxSoundId id, PcmSample sample);
  void set_rolloffs(const RolloffConfig& cars, const RolloffConfig& traffic) {
    car_rolloff_ = cars;
    traffic_rolloff_ = traffic;
  }
  void set_backfire_config(const BackfireConfig& cfg) { backfire_cfg_ = cfg; }

  // Audio-thread trigger entry points (lock-free, no heap allocation).
  void trigger_collision(const CollisionHit& hit, const VehicleAudioState& st);
  void trigger_ui(UiCue cue);

  // Audio-thread render: adds active SFX & 3D AI/traffic car audio into the
  // per-bus stereo buffers `buses[0..kBuses-1]` (interleaved stereo, `frames`
  // long; any bus pointer may be null). Calls `focus_engine->trigger_expander()`
  // when an exhaust backfire/burble pops.
  void render(const VehicleAudioState& st, float* const* buses, int frames, EnginePlayer* focus_engine = nullptr);

  int active_one_shots() const;
  int loaded_sample_count() const;
  uint32_t backfire_count() const { return backfire_count_.load(std::memory_order_relaxed); }

 private:
  struct DecodeJob {
    SfxSoundId id = SfxSoundId::kWind;
    std::filesystem::path bank_path;
    uint32_t subsound = 0;
  };
  struct LoadedSample {
    SfxSoundId id = SfxSoundId::kWind;
    PcmSample* pcm = nullptr;
  };
  struct LoopVoice {
    double phase = 0.0;
    float gain = 0.0f;
    float left = 1.0f, right = 1.0f;
    float pitch = 1.0f;
  };
  struct OneShot {
    bool active = false;
    SfxSoundId id = SfxSoundId::kWind;
    Bus bus = Bus::kUi;
    double phase = 0.0;
    float gain = 1.0f;
    float left = 1.0f, right = 1.0f;
    float pitch = 1.0f;
  };

  void start_decoder_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir);
  void stop_decoder_worker();
  void drain_loaded();
  void spawn_one_shot(SfxSoundId id, Bus bus, float gain, float left, float right, float pitch = 1.0f);
  void mix_loop(LoopVoice& v, SfxSoundId id, float target_gain, float target_pitch, float target_l, float target_r,
                float* bus, int frames);

  int rate_ = 48000;
  float smooth_ = 0.01f;

  TiresConfig tires_cfg_{};
  CollisionAudioConfig coll_cfg_{};
  UiAudioMap ui_map_{};
  BackfireConfig backfire_cfg_{};
  RolloffConfig car_rolloff_{};
  RolloffConfig traffic_rolloff_{};

  std::array<PcmSample*, int(SfxSoundId::kCount)> samples_{};
  SpscQueue<LoadedSample> loaded_q_{128};
  std::atomic<bool> stop_worker_{false};
  std::thread worker_;

  // Continuous loop voices.
  LoopVoice wind_voice_{};
  LoopVoice tire_roll_voice_{};
  LoopVoice tire_skid_voice_{};
  LoopVoice trans_voice_{};
  LoopVoice turbo_voice_{};
  LoopVoice sc_voice_{};
  std::array<LoopVoice, VehicleAudioState::kMaxOtherCars> other_voices_{};

  // Edge detection & timers (audio thread).
  bool prev_shifting_ = false;
  bool prev_bov_open_ = false;
  std::array<uint8_t, 4> prev_wheel_surface_{0, 0, 0, 0};
  std::array<double, 4> susp_cooldown_s_{0, 0, 0, 0};
  double time_s_ = 0.0;
  double last_collision_s_ = -10.0;
  double backfire_timer_ms_ = 500.0;
  std::atomic<uint32_t> backfire_count_{0};
  std::mt19937 rng_{0x51C2u};

  static constexpr int kMaxOneShots = 24;
  std::array<OneShot, kMaxOneShots> one_shots_{};
};

}  // namespace audio
