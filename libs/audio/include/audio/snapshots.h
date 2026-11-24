// FH1 9x22 mixer snapshot matrix (AudioMixerSnapshots.xml), DynamicsData
// triggers, GlobalDSPConfig.xml slewed car-bus parametric EQ, and
// AudioEngineConfig.xml master RMS compressor / saturator profiles.
// Spec: SPEC-AUDIO-EVENTS Part 3.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "audio/mixer.h"

namespace audio {

enum class Category : int {
  kFocusCar = 0,
  kOppCar,
  kTransmission,
  kSuperCharger,
  kTurbo,
  kEngineDamage,
  kBackfire,
  kSuspension,
  kCarWind,
  kTire,
  kCollision,
  kAmbientStatic,
  kAmbientDynamic,
  kGameMusic,
  kHomespaceAmbientDynamic,
  kHomespaceMusic,
  kMenuMusic,
  kGameUI,
  kVO,
  kSatNav,
  kDJModifier,
  kRallyCoDriver,
};
inline constexpr int kCategories = 22;

const char* category_name(Category c);
bool category_from_name(std::string_view name, Category& out);
Bus category_to_bus(Category c);

enum class SnapshotGroup : int {
  kGameplay = 0,
  kLocation,
  kSpeed,
  kImpact,
  kSpeedZones,
  kVO,
  kReverb,
  kRadio,
  kSatNav,
};
inline constexpr int kSnapshotGroups = 9;

const char* snapshot_group_name(SnapshotGroup g);
bool snapshot_group_from_name(std::string_view name, SnapshotGroup& out);

struct DynamicsData {
  float crash_threshold_mph = 50.0f;
  float crash_drop_mph = 10.0f;
  float top_speed_threshold = 0.70f;
  float music_hp_fadein = 3.0f;
  float music_hp_fadeout = 33.0f;
  float music_hp_cutoff = 200.0f;
};

struct SnapshotEntry {
  bool active = false;
  float dest_volume = 1.0f;
  float fade_time = 0.0f;
  float return_time = 0.0f;
  float return_fade_time = 0.0f;
};

struct MixSnapshotDef {
  std::string name;
  SnapshotGroup group = SnapshotGroup::kGameplay;
  std::array<SnapshotEntry, kCategories> categories{};
};

// 9-group x 22-category linear gain matrix with per-cell fade/hold/return
// ramps, priority overrides, transient Gameplay save/restore, and
// DynamicsData auto-triggers (SPEC-AUDIO-EVENTS Part 3).
class SnapshotMatrix {
 public:
  SnapshotMatrix();

  bool parse(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr,
            const std::filesystem::path& state_root = {});

  int snapshot_count() const { return int(defs_.size()); }
  int category_count() const { return category_count_; }
  const DynamicsData& dynamics() const { return dynamics_; }

  // Activates a snapshot by name (case-insensitive); applies override rules.
  bool activate(std::string_view name);
  // Restores the last non-transient Gameplay snapshot (e.g. exiting Paused/Rewind/Loading).
  void restore_gameplay();
  std::string_view active_name(SnapshotGroup g) const;

  // Rally stage radio routing: attenuates GameMusic cleanly during Rally stages
  // while keeping RallyCoDriver clear (1.0).
  void set_radio_rally_mode(bool on, float fade_s = 0.20f);
  bool radio_rally_mode() const { return radio_rally_mode_; }

  // Automatic CrashData and TopSpeedData triggers.
  void update_dynamics(double dt, float focus_speed_mph, float car_top_speed_mph = 140.0f);
  void reset_dynamics();

  // Advances all 9x22 cell fades and hold/return timers by `dt` seconds.
  void advance(double dt);

  // Final clamped product across all 9 groups for category `c`.
  float gain(Category c) const;
  // Representative gain for an 8-bus mixer bus.
  float bus_gain(Bus b) const;

  // Direct lookup for tests/inspection.
  int find(std::string_view name) const;
  const MixSnapshotDef* def(int idx) const {
    return idx >= 0 && size_t(idx) < defs_.size() ? &defs_[size_t(idx)] : nullptr;
  }

 private:
  enum class Phase : uint8_t { kIdle, kFadeIn, kHold, kReturnFade };
  struct Cell {
    float current = 1.0f;
    float start = 1.0f;
    float target = 1.0f;
    float elapsed = 0.0f;
    float duration = 0.0f;
    float hold_time = 0.0f;
    float return_fade = 0.0f;
    Phase phase = Phase::kIdle;
  };

  void apply_def(int idx);
  static bool is_transient_gameplay(std::string_view name);

  std::vector<MixSnapshotDef> defs_;
  int category_count_ = 21;
  DynamicsData dynamics_{};
  std::array<std::array<Cell, kCategories>, kSnapshotGroups> cells_{};
  std::array<int, kSnapshotGroups> active_idx_{};
  int saved_gameplay_idx_ = -1;

  // DynamicsData state.
  bool have_prev_speed_ = false;
  float prev_speed_mph_ = 0.0f;
  bool crash_latched_ = false;
  bool crash_saw_slow_ = false;
  bool in_top_speed_ = false;

  // Radio Rally Mode state (attenuates GameMusic while preserving RallyCoDriver).
  bool radio_rally_mode_ = false;
  float radio_rally_gain_ = 1.0f;
  float radio_rally_start_ = 1.0f;
  float radio_rally_target_ = 1.0f;
  float radio_rally_elapsed_ = 0.0f;
  float radio_rally_duration_ = 0.0f;
};

// GlobalDSPConfig.xml: car-bus peaking EQ for Rewind, 3-2-1 countdown, and
// Cockpit (all 4 cockpit bands bypassed on disc).
struct GlobalDspBand {
  bool bypass = true;
  float gain = 1.0f;
  float bandwidth = 1.0f;  // octaves
  float center = 1000.0f;  // Hz
};

struct GlobalDspConfig {
  GlobalDspBand rewind{};
  GlobalDspBand countdown{};
  float countdown_start_s = 6.5f;
  float countdown_end_s = 9.5f;
  std::array<GlobalDspBand, 4> cockpit{};

  bool parse(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr,
            const std::filesystem::path& state_root = {});
};

// Slewed stereo peaking EQ on AllCarsEmissionGroup (SPEC-AUDIO-EVENTS:
// Center 44,000 Hz/s, Bandwidth 10 oct/s, Gain 6.0 /s).
class CarBusEq {
 public:
  void prepare(int rate);
  void set_target(const GlobalDspBand& band);
  void process(float* stereo, int frames);
  float center() const { return center_; }
  float bandwidth() const { return bandwidth_; }
  float gain() const { return gain_; }
  bool bypassed() const { return bypass_ && std::abs(gain_ - 1.0f) < 1e-4f; }

 private:
  void update_coeffs();
  int rate_ = 48000;
  bool bypass_ = true;
  float target_center_ = 1000.0f, target_bw_ = 1.0f, target_gain_ = 1.0f;
  float center_ = 1000.0f, bandwidth_ = 1.0f, gain_ = 1.0f;
  // Biquad coefficients & stereo DF-II transposed state.
  float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
  float z1_[2]{}, z2_[2]{};
};

// Master RMS compressor & soft saturator parameters from AudioEngineConfig.xml.
struct MasterDspConfig {
  bool compressor_bypass = false;
  float compressor_level = 1.0f;
  float compressor_threshold = 0.1f;
  float compressor_attack = 0.05f;
  float compressor_release = 0.5f;
  float compressor_ratio = 0.25f;
  float compressor_gain_makeup = 1.0f;

  bool compressor_bypass_tv = false;
  float compressor_level_tv = 1.0f;
  float compressor_threshold_tv = 0.1f;
  float compressor_attack_tv = 0.05f;
  float compressor_release_tv = 0.5f;
  float compressor_ratio_tv = 0.15f;
  float compressor_gain_makeup_tv = 1.0f;

  bool saturator_bypass = false;
  float saturator_level = 0.95f;
  float saturator_saturation = 0.90f;
  float saturator_gain = 1.0f;

  bool parse(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr,
            const std::filesystem::path& state_root = {});
};

// Exact FH1 12-block (256-sample) RMS compressor (with LFE channel bypass)
// and soft saturator (SPEC-AUDIO-EVENTS Part 3).
float master_saturate(float x, float level, float saturation, float gain);

// Speaker/playback output profile governing the master RMS compressor curve
// derived from AudioEngineConfig.xml.
enum class PlaybackSystem : uint8_t {
  kHomeTheater = 0,       // AudioEngineConfig.xml normal profile (ratio 0.25)
  kTv = 1,                // AudioEngineConfig.xml TV profile (ratio 0.15)
  kHeadphones = 2,        // Near-field gentle dynamics (wider threshold, gentler ratio)
  kCarAudio = 3,          // Cabin-tuned dynamics (tighter threshold, slight makeup)
  kFullDynamicRange = 4,  // Compressor bypassed (soft saturator/limiter only)
};

class MasterRmsStage {
 public:
  static constexpr int kBlockSize = 256;
  static constexpr int kRmsBlocks = 12;

  void prepare(int rate);
  void set_config(const MasterDspConfig& cfg);
  void set_playback_system(PlaybackSystem mode);
  PlaybackSystem playback_system() const { return playback_system_; }
  void set_tv_profile(bool tv) {
    set_playback_system(tv ? PlaybackSystem::kTv : PlaybackSystem::kHomeTheater);
  }
  void set_full_dynamic_range(bool on) { full_range_ = on; }
  bool full_dynamic_range() const { return full_range_ || playback_system_ == PlaybackSystem::kFullDynamicRange; }

  // Processes interleaved stereo (`channels = 2`) or 5.1 (`channels = 6`,
  // where channel index 3 is LFE and passes the compressor untouched).
  void process(float* io, int frames, int channels = 2);
  float current_gain() const { return target_gain_; }
  float gain_reduction_db() const;
  float peak() const { return peak_; }

 private:
  void process_block(float* io, int n, int channels);

  MasterDspConfig cfg_{};
  int rate_ = 48000;
  PlaybackSystem playback_system_ = PlaybackSystem::kHomeTheater;
  bool full_range_ = false;
  std::array<float, kRmsBlocks> rms_ring_{};
  int rms_pos_ = 0;
  float prev_gain_ = 1.0f;
  float target_gain_ = 1.0f;
  float peak_ = 0.0f;
};

}  // namespace audio
