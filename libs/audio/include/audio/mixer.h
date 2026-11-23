// The mixer's pieces (ours; the buses, the snapshot states and the master
// stage). Spec: SPEC-RADIO "Ducking" and SPEC-ENGINE-SOUND "Category
// level" (snapshots multiply, each fading to its level over its fade time);
// the category names are the roadmap's. AUDIO_EVENTS.md will name the
// shipped snapshots; until then the built-in ones below stand in.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace audio {

// The category buses every sound belongs to.
// First 8 indices preserve the player-facing mix buses; indices 8..16 separate
// FH1's 22 AudioMixerSnapshots.xml subsystems so per-subsystem ducking works authentically.
enum class Bus : int {
  kEngine = 0,
  kOtherCars,
  kTyres,
  kWind,
  kAmbience,
  kMusic,
  kUi,
  kVo,
  kTransmission,
  kSupercharger,
  kTurbo,
  kEngineDamage,
  kBackfire,
  kSuspension,
  kCollision,
  kSatNav,
  kRallyCoDriver,
};
inline constexpr int kBuses = 17;

const char* bus_name(Bus b);
bool bus_from_name(std::string_view name, Bus& out);  // case-insensitive

// A snapshot: each bus's level while it is fully applied, and the fade in
// and back (seconds).
struct Snapshot {
  std::string name;
  std::array<float, kBuses> dest;
  float fade = 0.25f, return_fade = 0.25f;
  Snapshot() { dest.fill(1.0f); }
};

// The snapshots, each a weight 0..1 moving linearly to its target over its
// fade time. A bus's level is the product over snapshots of
// lerp(1, dest, weight): active snapshots multiply.
class SnapshotMixer {
 public:
  int add(Snapshot s);  // before use on the audio thread; returns its id
  int find(std::string_view name) const;  // -1 if none
  int count() const { return int(snaps_.size()); }
  void set_active(int id, bool on);
  bool active(int id) const;
  void advance(double dt);
  float level(Bus b) const;
  // The built-in states: "Paused", "Menu", "Title", "Loading".
  void add_defaults();

 private:
  struct State {
    Snapshot snap;
    bool on = false;
    float weight = 0;
  };
  std::vector<State> snaps_;
};

struct CompressorSettings {
  float threshold_db = -9.0f;
  float ratio = 3.0f;
  float knee_db = 6.0f;
  float attack_ms = 8.0f;
  float release_ms = 160.0f;
  float makeup_db = 0.0f;
};

// A stereo-linked feed-forward compressor with a soft knee, then a soft
// limiter at full scale. Bypassed (Full Dynamic Range) it lets the level
// through to the limiter alone.
class MasterStage {
 public:
  void prepare(int rate);
  void set_settings(const CompressorSettings& s);
  void set_full_dynamic_range(bool on) { full_range_ = on; }
  bool full_dynamic_range() const { return full_range_; }
  // In place, interleaved stereo.
  void process(float* io, int frames);
  float gain_reduction_db() const { return gr_db_; }
  float peak() const { return peak_; }  // of the last block, after the limiter

 private:
  CompressorSettings s_;
  int rate_ = 48000;
  bool full_range_ = false;
  float gr_db_ = 0;  // the smoothed reduction, dB (>= 0)
  float peak_ = 0;
  float attack_ = 0, release_ = 0;
};

// The soft limiter's curve on one sample (identity under 0.9, tends to 1).
float soft_limit(float x);

// Buses -> master: per-bus user level (Sound Mix, 0..2) times the
// snapshots' level, master volume and mute, all smoothed over ~10 ms, then
// the master stage.
class Mixer {
 public:
  void prepare(int rate);
  void set_bus_level(Bus b, float level) { user_[size_t(b)] = level < 0 ? 0 : level; }
  float bus_level(Bus b) const { return user_[size_t(b)]; }
  void set_master(float v) { master_ = v < 0 ? 0 : v; }
  float master() const { return master_; }
  void set_mute(bool m) { mute_ = m; }
  bool muted() const { return mute_; }
  SnapshotMixer& snapshots() { return snaps_; }
  const SnapshotMixer& snapshots() const { return snaps_; }
  MasterStage& stage() { return stage_; }
  const MasterStage& stage() const { return stage_; }
  // Mixes the buses' blocks (`bus[b]`: interleaved stereo, `frames` long,
  // may be null: silent) into `out` (overwritten) and advances the
  // snapshots by the block's time.
  void mix(const float* const* bus, int frames, float* out);

 private:
  int rate_ = 48000;
  std::array<float, kBuses> user_;
  std::array<float, kBuses> smooth_;
  float master_ = 1, master_smooth_ = 1;
  bool mute_ = false;
  float mute_smooth_ = 1;
  SnapshotMixer snaps_;
  MasterStage stage_;
  std::vector<float> acc_;

 public:
  Mixer() {
    user_.fill(1.0f);
    smooth_.fill(1.0f);
  }
};

}  // namespace audio
