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
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/engine_player.h"
#include "audio/event_configs.h"
#include "audio/fev.h"
#include "audio/mixer.h"
#include "audio/rt.h"
#include "audio/ui_audio.h"

namespace audio {

struct OtherCarAudio {
  bool active = false;
  bool is_traffic = false;
  uint8_t variant = 0;  // selects among distinct LOD1 opponent engine loop banks
  Vec3f pos{};
  Vec3f vel{};
  float rpm = 1000.0f;
  float speed_mph = 0.0f;
  float pitch_scale = 1.0f;        // per-car engine fundamental timbre offset [0.78..1.28]
  float slip = 0.0f;               // 0..1 cornering/braking tyre scrub for LOD2 tyre audio
  uint32_t car_id = 0;             // stable unique car ID for persistent 3D voice binding (0 = slot fallback)
  uint8_t archetype = 0;           // 0=Auto/I4, 1=V6, 2=V8, 3=V10/V12, 4=Turbo, 5=EV, 6=Traffic
  float fov_rolloff_scale = 1.0f;  // replay FOV zoom rolloff scale (applied in RolloffCurve::eval, not position)
  uint8_t surface_group = 0;       // 0=OnRoad, 1=OffRoad/Gravel, 2=Grass, 3=Brick, 4=Trackway, 5=Mud
};

struct WheelAudio {
  uint8_t surface_group = 0;  // 0=OnRoad, 1=OffRoad/Gravel, 2=Grass, 3=Brick, 4=Trackway, 5=Mud
  float slip_combined = 0.0f;
  float wheel_rpm = 0.0f;
  float load = 1.0f;
  float bottom_out_force = 0.0f;
  int16_t surface_index = -1;  // Tires.xml SurfaceIndex (>= 0 preserves distinct channels; -1 = derive from group)
  float slip_ratio = 0.0f;     // longitudinal slip magnitude (burnouts / braking)
  float slip_angle = 0.0f;     // lateral slip magnitude (cornering scrub)
};

struct ShowcaseAudioState {
  bool active = false;
  uint8_t kind = 0;  // 0=MustangP51, 1=HotAirBalloon, 2=StuntJet, 3=Biplane, 4=Helicopter
  Vec3f pos{};
  Vec3f vel{};
  float speed_mps = 0.0f;
  float bank = 0.0f;   // roll angle (rad)
  float pitch = 0.0f;  // pitch angle (rad)
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
  int trans_level = 0;  // 0=L0, 1=L1, 2=L2, 3=L3
  int turbo_level = 0;  // 0=None, 1=L1, 2=L2, 3=L3
  int sc_level = 0;     // 0=None, 1=L1, 2=L2, 3=L3
  bool sc_is_centrifugal = false;
  std::array<int, 3> blowoff_level{1, 2, 3};   // per-car _ET.xml blowoffL1..L3Level (1..3)
  std::array<int, 3> blowoff_choice{1, 1, 1};  // per-car _ET.xml blowoffL1..L3Choice (1..5)
  int backfire_level = 0;
  bool rally_mode = false;  // selects Rally_Tires.fev/.fsb surfaces when loaded
  bool scraping = false;
  float scrape_intensity = 0.0f;
  std::array<WheelAudio, 4> wheels{};
  Vec3f listener_pos{};
  Vec3f listener_vel{};
  Vec3f listener_right{1.0f, 0.0f, 0.0f};
  bool cockpit_view = false;
  bool in_race = false;
  bool rewinding = false;
  bool music_active = false;
  bool is_electric = false;           // drives ElectricEngine.fev EV inverter/motor loop (e.g. NIS_Leaf_11)
  float reflection_proximity = 0.0f;  // 0..1 proximity to roadside walls/barriers/canyons (Reflections.fev)
  float active_aero_deploy = 0.0f;    // 0..1 active rear wing / airbrake deployment state (ActiveAero.fev)
  // Per-car _ET.xml VolumeAdjustments & cylinder count (populated by AudioHost::load_car).
  float trans_vol_scale = 1.0f;
  float shift_vol_scale = 1.0f;
  float turbo_vol_scale = 1.0f;
  float sc_vol_scale = 1.0f;
  float backfire_vol_scale = 1.0f;
  float burble_vol_scale = 0.0f;
  float exh_noise_vol_scale = 0.0f;
  float susp_vol_scale = 1.0f;
  float ev_vol_scale = 1.0f;
  int num_cylinders = 0;
  // Mechanical damage levels (0..1) driving Damage.fev rattle/knock/misfire on Bus::kEngineDamage.
  float engine_damage = 0.0f;
  float drivetrain_damage = 0.0f;
  // Per-car _HT.xml <Burble> overrun timer parameters (distinct from shift/decel Backfire).
  float burble_min_time_ms = 110.0f;
  float burble_max_time_ms = 280.0f;
  float burble_min_norm_rpm = 0.25f;
  float burble_max_norm_rpm = 0.95f;
  float burble_silence_weight = 60.0f;
  // Per-view _CMT.xml <OtherVolumes> isolation multipliers (applied when cockpit_view/follow active).
  float view_wind_scale = 1.0f;
  float view_tyre_scale = 1.0f;
  float view_trans_scale = 1.0f;
  float view_turbo_scale = 1.0f;
  float view_sc_scale = 1.0f;
  float view_backfire_scale = 1.0f;
  float view_burble_scale = 1.0f;
  float view_susp_scale = 1.0f;
  float view_ambience_scale = 1.0f;
  float view_other_cars_scale = 1.0f;
  float view_damage_scale = 1.0f;
  float view_exh_noise_scale = 1.0f;
  float view_ev_scale = 1.0f;
  float view_lfe_scale = 1.0f;
  float view_refl_scale = 1.0f;
  uint8_t hub_ambience = 0;  // 0=None, 1=Autoshow, 2=Paintshop, 3=Workshop
  std::array<OtherCarAudio, kMaxOtherCars> other_cars{};
  ShowcaseAudioState showcase{};
  // R3-AUD-01 / R3-AUD-12 extended vehicle telemetry (defaults preserve backward compatibility).
  float gear_ratio = 0.0f;         // active gearbox total ratio (gear * final_drive; 0 = synthesize from gear)
  float rewind_speed = 0.0f;       // |playback_speed| while rewinding (0 = default 1.0 while rewinding=true)
  float rewind_progress = 0.0f;    // 0..1 progress across active rewind window
  uint8_t scrape_surface_hint = 0; // 0=Asphalt, 1=Car, 2=Dirt/Grass, 3=Metal/Guardrail, 4=Wood/Prop
  uint8_t glass_broken_mask = 0;   // bit0=windshield, bit1=side, bit2=rear, bit3=taillight/lamp, bit4=lexan
};

enum class SmashMaterial : uint8_t {
  kDefault = 0,
  kWood,
  kMetal,
  kGlass,
  kPlastic,
  kFoliage,
  kStone,
  kFence,
  kCount,
};

struct CollisionHit {
  float rel_speed_mps = 0.0f;
  Vec3f pos{};
  bool is_car_on_car = false;
  bool is_break = false;
  uint8_t surface_hint = 0;  // 0=Asphalt/Concrete, 1=Car, 2=Dirt/Grass, 3=Metal/Guardrail, 4=Wood/Prop
  SmashMaterial smash_material = SmashMaterial::kDefault;
};

struct LandingHit {
  float impact_speed_mps = 0.0f;  // vertical touchdown speed magnitude (m/s)
  float air_time_s = 0.0f;        // seconds airborne before landing
  Vec3f pos{};
  uint8_t surface_hint = 0;       // 0=Asphalt/Hard, 1=Dirt/Gravel, 2=Grass, 3=Mud
};

enum class UiCue : uint8_t {
  kAccept = 0,
  kCancel,
  kMove,
  kError,
  kMenuOpen,
  kRewindTapeStart,
  kCountdown321,
  kCountdownGo,
  kCheckpoint,
  kLap,
  kRaceFinish,
  kSignSmash,
  kSkillAward,
  kSkillMultiplier,
  kSkillBank,
  kSkillBreak,
  kRankUp,
  kAchievementReward,
  kSpeedCamera,
  kPhotoShutter,
  kPurchase,
  kCarAdded,
  kH2hGo,
  kH2hLoss,
  kRewindTapeStop,
  kCountdownTick,
  kWristbandUnlock,
};

enum class SfxSoundId : int {
  kWind = 0,
  kTireRollOnRoad,
  kTireSkidOnRoad,
  kTireRollOffRoad,
  kTireSkidOffRoad,
  kTireRollGrass,
  kTireSkidGrass,
  kTireRollBrick,
  kTireSkidBrick,
  kTireRollTrackway,
  kTireSkidTrackway,
  kSurfaceHardToHard,
  kSurfaceDirtToHard,
  kTransWhine,
  kTransWhineL1,
  kTransWhineL2,
  kTransWhineL3,
  kShiftStart,
  kShiftComplete,
  kTurboSpool,
  kTurboSpoolL2,
  kTurboSpoolL3,
  kTurboBovLo,
  kTurboBovHi,
  kTurboBovLoL2,
  kTurboBovHiL2,
  kTurboBovLoL3,
  kTurboBovHiL3,
  kScWhine,
  kScWhineL2,
  kScWhineL3,
  kScCscL1,
  kScCscL2,
  kScCscL3,
  kEngineLfeLoop,
  kSuspensionBottom,
  kImpact3d,
  kImpactLfe,
  kBreakProp,
  kScrapeLoop,
  kWhoosh3d,
  kGlassShatter,
  kBackfirePop,
  kBackfirePop1,
  kBackfirePop2,
  kBackfirePop3,
  kBackfireLfe,
  kUiAccept,
  kUiCancel,
  kUiMove,
  kUiError,
  kUiMenuOpen,
  kRewindTapeStart,
  kRewindLoop,
  kCountdown321,
  kSignSmash,
  kUiSkillAward,
  kAchievementReward,
  kUiSpeedCamera,
  kOtherCarLoop,
  kOtherCarLoop1,
  kOtherCarLoop2,
  kOtherCarLoop3,
  kTrafficCarLoop,
  kShowcaseP51,
  kShowcaseBiplane,
  kShowcaseHeli,
  kShowcaseBalloon,
  kShowcaseJet,
  kElectricMotor,
  kRoadsideReflection,
  kActiveAero,
  kShiftStartL1,
  kShiftStartL2,
  kShiftStartL3,
  kShiftCompleteL1,
  kShiftCompleteL2,
  kShiftCompleteL3,
  kEngineLfe4,
  kEngineLfe6,
  kEngineLfe8,
  kEngineLfe10,
  kEngineLfe12,
  kImpactCarOnCar,
  kImpactDirt,
  kImpactMetal,
  kImpactWood,
  kUiCountdownGo,
  kUiCheckpoint,
  kUiRaceFinish,
  kUiSkillMulti,
  kUiSkillBank,
  kUiSkillBreak,
  kUiRankUp,
  kUiPhotoShutter,
  kUiPurchase,
  kUiCarAdded,
  kUiH2hGo,
  kUiH2hLoss,
  kHubAmbAutoshow,
  kHubAmbPaintshop,
  kHubAmbWorkshop,
  // Per-material smashable prop banks (Collisions.fev BreakEvent variants).
  kBreakWood,
  kBreakMetal,
  kBreakGlass,
  kBreakPlastic,
  kBreakFoliage,
  kBreakStone,
  kBreakFence,
  // Per-car Turbo BOV choices 2..5 for L1..L3 Lo/Hi (Turbos.fev).
  kTurboBovL1C2Lo,
  kTurboBovL1C2Hi,
  kTurboBovL1C3Lo,
  kTurboBovL1C3Hi,
  kTurboBovL1C4Lo,
  kTurboBovL1C4Hi,
  kTurboBovL1C5Lo,
  kTurboBovL1C5Hi,
  kTurboBovL2C2Lo,
  kTurboBovL2C2Hi,
  kTurboBovL2C3Lo,
  kTurboBovL2C3Hi,
  kTurboBovL2C4Lo,
  kTurboBovL2C4Hi,
  kTurboBovL2C5Lo,
  kTurboBovL2C5Hi,
  kTurboBovL3C2Lo,
  kTurboBovL3C2Hi,
  kTurboBovL3C3Lo,
  kTurboBovL3C3Hi,
  kTurboBovL3C4Lo,
  kTurboBovL3C4Hi,
  kTurboBovL3C5Lo,
  kTurboBovL3C5Hi,
  // Horizon Rally DLC subsystems: Rally_Tires.fev/.fsb and Car_Landings.fev/.fsb.
  kRallyTireRollGravel,
  kRallyTireSkidGravel,
  kRallyTireRollDirt,
  kRallyTireSkidDirt,
  kRallyTireRollMud,
  kRallyTireSkidMud,
  kRallyTireRollTarmac,
  kRallyTireSkidTarmac,
  kCarLandingLight,
  kCarLandingHeavy,
  kCarLandingDirt,
  // Phase R2-5 completeness: SurfaceChanges Leaves, Rewind TapeStop, CountdownTick,
  // distinct Lap & Wristband UI stings, Burble overrun crackle pool, ExhNoise,
  // Damage rattle/misfire, and multi-band ElectricMotor layers.
  kSurfaceLeavesOn,
  kSurfaceLeavesOff,
  kRewindTapeStop,
  kUiCountdownTick,
  kUiLap,
  kUiWristbandUnlock,
  kBurblePop0,
  kBurblePop1,
  kBurblePop2,
  kBurblePop3,
  kExhNoise,
  kEngineDamageLoop,
  kEngineMisfire,
  kElectricMotorMid,
  kElectricMotorHigh,
  // R3-AUD-06: 2-band high-RPM opponent/traffic layers + Turbo archetype bank.
  kOtherCarLoopHi,
  kOtherCarLoop1Hi,
  kOtherCarLoop2Hi,
  kOtherCarLoop3Hi,
  kOtherCarLoop4,
  kOtherCarLoop4Hi,
  kTrafficCarLoopHi,
  // R3-AUD-12: All 5 Glass.fev shatter variants + surface-specific scrape loops.
  kGlassSideWindow,
  kGlassRearWindow,
  kGlassTaillight,
  kGlassLexan,
  kScrapeMetal,
  kScrapeDirt,
  kScrapeWood,
  // Damage.fev DamageEngine's second instance (DamFast; kEngineDamageLoop is DamSlow).
  kEngineDamageFast,
  kCount
};

// Maps Turbo BOV (level 1..3, choice 1..5, hi = >13 PSI) to its SfxSoundId.
SfxSoundId turbo_bov_sound_id(int level_1_to_3, int choice_1_to_5, bool hi);
// Maps SmashMaterial (with optional CollisionHit::surface_hint fallback) to its SfxSoundId.
SfxSoundId smash_material_sound_id(SmashMaterial mat, uint8_t surface_hint = 0);

class SfxEngine {
 public:
  explicit SfxEngine(int out_rate = 48000);
  ~SfxEngine();

  // Parses .fev projects and XML tables from `game_root` (layering TU4 overrides
  // and Rally DLC banks when `state_root` is provided), then starts an async
  // background worker decoding referenced FSB4 subsounds into `cache_dir`.
  bool load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
            const std::string& burble_bank = {}, std::string* note = nullptr,
            const std::filesystem::path& state_root = {});
  // Discovers and asynchronously decodes Rally DLC `Rally_Tires.fev/.fsb` and
  // `Car_Landings.fev/.fsb` from a Rally package dir, audio dir, or `state_root`.
  bool load_rally_sfx(const std::filesystem::path& rally_root_or_state_root,
                      const std::filesystem::path& cache_dir, std::string* note = nullptr);
  // Asynchronously reloads the exhaust burble / backfire bank on car change.
  void reload_burble_bank(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                          const std::string& burble_bank, int burble_count = 0, int backfire_count = 0);

  // Injects a PCM sample directly (for unit tests without game data).
  void set_sample(SfxSoundId id, PcmSample sample);
  // Populates clean procedural SFX fallbacks for core slots (Android / no-ffmpeg fallback).
  void populate_procedural_fallbacks();
  void set_rolloffs(const RolloffConfig& cars, const RolloffConfig& traffic) {
    car_rolloff_ = cars;
    traffic_rolloff_ = traffic;
  }
  void set_backfire_config(const BackfireConfig& cfg) { backfire_cfg_ = cfg; }
  void set_tires_config(const TiresConfig& cfg) { tires_cfg_ = cfg; }
  void set_collision_config(const CollisionAudioConfig& cfg) { coll_cfg_ = cfg; }
  // Maps a physics surface name via Tires.xml (0=OnRoad, 1=OffRoad, 2=Grass, 3=Brick, 4=Trackway, 5=Mud).
  uint8_t map_surface_group(std::string_view physics_surface) const;
  // Maps a physics surface name via Tires.xml to its distinct SurfaceIndex (-1 if unknown).
  int16_t map_surface_index(std::string_view physics_surface) const;

  // Audio-thread trigger entry points (lock-free, no heap allocation).
  void trigger_collision(const CollisionHit& hit, const VehicleAudioState& st);
  void trigger_landing(const LandingHit& hit, const VehicleAudioState& st);
  void trigger_ui(UiCue cue);
  // Plays UI catalog sample slot `sample` (one layer of a ui4audio event) once
  // on the UI bus. The host resolves the play name and scene to the event and
  // sends each of its layers, so the audio thread never reads the catalog.
  void trigger_ui_sample(int sample, float gain, float left, float right);

  // The ui4audio catalog load() built (null before load or without the disc's
  // ui4audio.xml / UIInGame.fev). Immutable once published; read it on the
  // control thread before handing the engine to the audio thread.
  std::shared_ptr<const UiAudioCatalog> ui_catalog() const { return ui_catalog_; }
  // Tests: install the sample for a catalog sample slot.
  void set_ui_sample(int sample_slot, PcmSample sample);
  bool is_ui_sample_active(int sample) const;
  int loaded_ui_sample_count() const;
  bool has_ui_sample(int sample) const {
    return sample >= 0 && sample < kMaxUiSamples && ui_samples_[size_t(sample)] && !ui_samples_[size_t(sample)]->data.empty();
  }
  static constexpr int kMaxUiSamples = 2048;

  // Audio-thread render: adds active SFX & 3D AI/traffic car audio into the
  // per-bus stereo buffers `buses[0..kBuses-1]` (interleaved stereo, `frames`
  // long; any bus pointer may be null). Calls `focus_engine->trigger_expander()`
  // when an exhaust backfire/burble pops. When `lfe_out != nullptr` (mono,
  // `frames` floats), routes EngineLFE, ImpactLFE, and BackfireLFE exclusively
  // into `lfe_out` instead of summing them into stereo L/R pre-compressor.
  void render(const VehicleAudioState& st, float* const* buses, int frames,
              EnginePlayer* focus_engine = nullptr, float* lfe_out = nullptr);

  int active_one_shots() const;
  bool is_one_shot_active(SfxSoundId id) const;
  uint32_t bound_other_car_id(int voice_slot) const;
  float last_lfe_pitch() const { return engine_lfe_voice_.pitch; }
  float last_rewind_pitch() const { return rewind_voice_.pitch; }
  int loaded_sample_count() const;
  uint32_t backfire_count() const { return backfire_count_.load(std::memory_order_relaxed); }
  uint32_t burble_count() const { return burble_count_.load(std::memory_order_relaxed); }
  SfxSoundId last_one_shot_id() const { return last_one_shot_id_.load(std::memory_order_acquire); }
  float last_one_shot_gain() const { return last_one_shot_gain_.load(std::memory_order_acquire); }
  float last_one_shot_pitch() const { return last_one_shot_pitch_.load(std::memory_order_acquire); }

 private:
  struct DecodeJob {
    SfxSoundId id = SfxSoundId::kWind;
    std::filesystem::path bank_path;
    uint32_t subsound = 0;
    int ui = -1;  // >= 0: the sample of UI catalog event `ui` instead of `id`
  };
  struct LoadedSample {
    SfxSoundId id = SfxSoundId::kWind;
    PcmSample* pcm = nullptr;
    int ui = -1;
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
    int ui = -1;  // >= 0: plays ui_samples_[ui] (a UI catalog event)
    Bus bus = Bus::kUi;
    double phase = 0.0;
    float gain = 1.0f;
    float left = 1.0f, right = 1.0f;
    float pitch = 1.0f;
    uint8_t priority = 50;
    uint64_t spawn_seq = 0;
    // Pending 5 ms fade-out before replacing a stolen voice.
    bool stealing = false;
    int fade_rem_frames = 0;
    int fade_total_frames = 0;
    SfxSoundId next_id = SfxSoundId::kWind;
    int next_ui = -1;
    Bus next_bus = Bus::kUi;
    float next_gain = 0.0f;
    float next_left = 0.707f, next_right = 0.707f;
    float next_pitch = 1.0f;
    uint8_t next_priority = 50;
    uint64_t next_spawn_seq = 0;
  };
  struct PersistentOtherVoice {
    uint32_t bound_car_id = 0;  // 0 = free
    bool active = false;
    bool fading_out = false;
    float fade_out_rem_s = 0.0f;
    float last_dist_m = 1e6f;
    SfxSoundId last_lo_id = SfxSoundId::kOtherCarLoop;
    SfxSoundId last_hi_id = SfxSoundId::kCount;
    SfxSoundId last_tire_id = SfxSoundId::kTireRollOnRoad;
    LoopVoice lo_voice{};
    LoopVoice hi_voice{};
    LoopVoice tire_voice{};
    double whoosh_cooldown_s = -10.0;
  };

  void start_decoder_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir,
                            std::filesystem::path game_root = {});
  void stop_decoder_worker();
  void drain_queue(SpscQueue<LoadedSample>& q);
  void drain_loaded();
  void reclaim_retired();
  void spawn_one_shot(SfxSoundId id, Bus bus, float gain, float left, float right, float pitch = 1.0f, int ui = -1);
  void mix_loop(LoopVoice& v, SfxSoundId id, float target_gain, float target_pitch, float target_l, float target_r,
                float* bus, int frames);
  void mix_loop_mono(LoopVoice& v, SfxSoundId id, float target_gain, float target_pitch, float* mono_out, int frames);

  int rate_ = 48000;
  float smooth_ = 0.01f;

  TiresConfig tires_cfg_{};
  CollisionAudioConfig coll_cfg_{};
  UiAudioMap ui_map_{};
  BackfireConfig backfire_cfg_{};
  RolloffConfig car_rolloff_{};
  RolloffConfig traffic_rolloff_{};

  std::array<PcmSample*, int(SfxSoundId::kCount)> samples_{};
  std::shared_ptr<const UiAudioCatalog> ui_catalog_;
  std::array<PcmSample*, kMaxUiSamples> ui_samples_{};  // by UI catalog event index
  // Dedicated SPSC queues (one per producer) so the real-time audio thread
  // drains all loaded samples lock-free with zero multi-producer races.
  SpscQueue<LoadedSample> loaded_q_{2048};        // producer: worker_
  SpscQueue<LoadedSample> burble_loaded_q_{32};   // producer: burble_worker_
  SpscQueue<LoadedSample> rally_loaded_q_{64};    // producer: rally_worker_
  SpscQueue<LoadedSample> direct_loaded_q_{512};  // producer: set_sample() (guarded by direct_q_mu_)
  SpscQueue<PcmSample*> retired_q_{512};          // producer: audio thread drain_queue(); consumer: control thread
  std::mutex direct_q_mu_;
  std::mutex reclaim_mu_;    // guards control-thread reclaim_retired() only (never locked on audio thread)
  std::mutex lifecycle_mu_;  // guards control-thread worker start/stop only (never locked on audio thread)
  std::atomic<bool> stop_worker_{false};
  std::thread worker_;
  std::atomic<bool> stop_burble_worker_{false};
  std::thread burble_worker_;
  std::atomic<bool> stop_rally_worker_{false};
  std::thread rally_worker_;

  // Continuous loop voices.
  LoopVoice wind_voice_{};
  std::array<LoopVoice, 4> tire_roll_voices_{};
  std::array<LoopVoice, 4> tire_skid_voices_{};
  LoopVoice trans_voice_{};
  LoopVoice turbo_voice_{};
  LoopVoice sc_voice_{};
  LoopVoice engine_lfe_voice_{};
  LoopVoice electric_voice_{};
  LoopVoice electric_mid_voice_{};
  LoopVoice electric_high_voice_{};
  LoopVoice exh_noise_voice_{};
  LoopVoice damage_voice_{};
  LoopVoice damage_fast_voice_{};
  // Damage.fev DamageEngine (read-only after load; evaluated on the audio thread).
  static constexpr int kDamageInstances = 2;
  FevEvent damage_event_{};
  bool damage_event_ok_ = false;
  std::array<SfxSoundId, kDamageInstances> damage_sound_ids_{SfxSoundId::kCount, SfxSoundId::kCount};
  int damage_param_damage_ = -1;
  int damage_param_rpm_ = -1;
  int damage_param_throttle_ = -1;
  // Real-time synth stand-ins (damage knock, showcase aircraft) run only in the
  // explicit no-assets mode set by populate_procedural_fallbacks().
  bool procedural_mode_ = false;
  LoopVoice reflection_voice_{};
  LoopVoice scrape_voice_{};
  LoopVoice rewind_voice_{};
  LoopVoice hub_amb_voice_{};
  std::array<PersistentOtherVoice, VehicleAudioState::kMaxOtherCars> other_slots_{};
  LoopVoice showcase_voice_{};
  double showcase_synth_phase_ = 0.0;
  double showcase_synth_lfo_ = 0.0;
  float showcase_synth_lp_ = 0.0f;
  uint32_t showcase_synth_rng_ = 0x5A0C45E1u;
  double damage_synth_phase_ = 0.0;
  float damage_synth_lp_ = 0.0f;
  uint32_t damage_synth_rng_ = 0xD4A6E109u;

  // Edge detection & timers (audio thread).
  SfxSoundId resolve_fallback(SfxSoundId preferred, SfxSoundId fallback) const;
  bool prev_shifting_ = false;
  int prev_gear_ = 1;
  bool prev_bov_open_ = false;
  struct ThrottleHistEntry {
    double time_s = 0.0;
    float throttle = 0.0f;
  };
  static constexpr int kThrottleHistSize = 16;
  std::array<ThrottleHistEntry, kThrottleHistSize> thr_hist_{};
  int thr_hist_head_ = 0;
  int thr_hist_count_ = 0;
  bool liftoff_armed_ = false;
  float prev_active_aero_ = 0.0f;
  double aero_cooldown_s_ = -10.0;
  double rewind_elapsed_s_ = 0.0;
  uint8_t prev_glass_broken_mask_ = 0;
  uint64_t one_shot_seq_ = 0;
  std::array<uint8_t, 4> prev_wheel_surface_{0, 0, 0, 0};
  std::array<int16_t, 4> prev_wheel_surface_idx_{-1, -1, -1, -1};
  std::array<double, 4> susp_cooldown_s_{0, 0, 0, 0};
  double time_s_ = 0.0;
  double last_collision_s_ = -10.0;
  double last_landing_s_ = -10.0;
  double backfire_timer_ms_ = 500.0;
  double burble_timer_ms_ = 200.0;
  std::atomic<uint32_t> backfire_count_{0};
  std::atomic<uint32_t> burble_count_{0};
  std::atomic<SfxSoundId> last_one_shot_id_{SfxSoundId::kCount};
  std::atomic<float> last_one_shot_gain_{0.0f};
  std::atomic<float> last_one_shot_pitch_{1.0f};
  std::mt19937 rng_{0x51C2u};

  static constexpr int kMaxOneShots = 24;
  std::array<OneShot, kMaxOneShots> one_shots_{};
};

}  // namespace audio
