// World Ambience & Acoustics (Slice 9):
// - Parses colorado_reverb.xml (24 I3DL2/SFX environmental reverb templates
//   including 12_PGTunnel with SnapshotMix="InTunnel" / "NotInTunnel").
// - Parses colorado_ambience.xml (6 regional ambience templates: Default,
//   MainTown, Mountains, RedRock, FunctionalArea, Festival).
// - Parses ColoradoRibbon_00.xml (Festival stage/crowd 3D emitters) and
//   Colorado's .soundscape XML tiles (<ReverbZones> with barycentric
//   Template0/Template1 interpolation, <AmbienceZones>, <Track3D>, <Crowds>).
// - Real-time stereo Schroeder/Freeverb-style environmental reverb DSP driven
//   by the interpolated I3DL2 parameters + regional/festival ambient bed.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "audio/engine_player.h"
#include "audio/fev.h"
#include "audio/rt.h"

namespace audio {

struct ReverbTemplate {
  std::string name;
  std::string snapshot_mix = "NotInTunnel";  // "InTunnel" or "NotInTunnel"
  float decay_time_s = 1.2f;
  float decay_hf_ratio = 1.0f;
  float density_pct = 100.0f;
  float diffusion_pct = 100.0f;
  float dry_level_mb = 0.0f;
  float room_level_mb = -800.0f;
  float room_hf_level_mb = -1500.0f;
  float reflections_level_mb = -3000.0f;
  float reflections_delay_s = 0.0f;
  float reverb_level_mb = 0.0f;
  float reverb_delay_s = 0.0f;
  float hf_reference_hz = 6260.0f;
  float near_dist = 10.0f;
  float far_dist = 60.0f;
  float near_dry = 1.0f;
  float near_wet = 0.0f;
  float far_dry = 0.8f;
  float far_wet = 0.8f;
};

struct AmbienceTemplate {
  std::string name;
  std::string snapshot_mix = "DefaultLocation";
  std::string background_stream_name;
  std::string background_stream_path;
  float start_below_speed_mph = 35.0f;
  float stop_above_speed_mph = 40.0f;
  std::string crowd_event_name;
  std::string tree_event_name;
};

struct ReverbTriangle {
  std::array<float, 3> x{};
  std::array<float, 3> z{};
  std::array<float, 3> value{};  // 0..1 blend from template0 to template1
};

struct ReverbZone {
  int template0 = 0;
  int template1 = 0;
  float min_height = -1e6f;
  float max_height = 1e6f;
  float min_x = 0, max_x = 0, min_z = 0, max_z = 0;
  std::vector<ReverbTriangle> triangles;
};

struct AmbienceTriangle {
  std::array<float, 3> x{};
  std::array<float, 3> z{};
};

struct AmbienceZone {
  int template_idx = 0;
  float min_height = -1e6f;
  float max_height = 1e6f;
  float min_x = 0, max_x = 0, min_z = 0, max_z = 0;
  std::vector<AmbienceTriangle> triangles;
};

struct AmbientEmitter3D {
  std::string object_name;
  std::string event_path;
  std::string event_name;
  std::array<float, 3> pos1{};
  std::array<float, 3> pos2{};
  float max_distance = 220.0f;
  uint32_t event_hash = 0;  // ambience_event_hash(event_name)
  // From the disc's event (WorldAcousticsData::set_event_reach): its
  // (distance) range (0: max_distance) and whether the player has a loop
  // for it (spawned one-shots and events the disc lacks have none).
  float reach_m = 0.0f;
  bool playable = true;
};

// An ambience event's reach: the range of its (distance) parameter (or its
// 3D max distance) and whether it has a continuous loop the bed player plays.
struct AmbienceEventReach {
  float distance_m = 0.0f;
  bool playable = false;
};

struct SoundscapeTile {
  std::vector<ReverbZone> reverb_zones;
  std::vector<AmbienceZone> ambience_zones;
  std::vector<AmbientEmitter3D> emitters;
  int tree_count = 0;
  int crowd_count = 0;
};

enum class AmbientEmitterKind : uint8_t {
  kGeneric = 0,
  kCrowd,
  kFestival,
  kWater,
  kTrees,
};

struct RtAmbientEmitter {
  bool active = false;
  AmbientEmitterKind kind = AmbientEmitterKind::kGeneric;
  float gain = 0.0f;  // distance attenuation [0..1]
  float pan = 0.0f;   // stereo pan [-1..1] relative to listener heading
  uint32_t id_hash = 0;
  uint32_t event_hash = 0;  // the emitter's FEV event name (ambience_event_hash)
  float distance_m = 0.0f;  // listener to emitter: the event's (distance) parameter
};

// Case-insensitive FNV-1a of an ambience event name ("Default_Crickets_Bed").
uint32_t ambience_event_hash(std::string_view event_name);

// The game state the ambience events read (SPEC-AUDIO-EVENTS, "World
// ambience"): AMB_Default.fev's CurrentTime (0..86,400 s), CarSpeed and
// Speed (mph), InRace, PreRaceAmbience, CameraType, RadioIsPlaying and
// Engine_RPM; Colorado_Festival.fev's CrowdExcitement.
struct AmbienceParams {
  float current_time_s = 43200.0f;
  float car_speed_mph = 0.0f;
  float speed = 0.0f;
  float in_race = 0.0f;
  float pre_race_ambience = 0.0f;
  float camera_type = 0.0f;
  float radio_is_playing = 0.0f;
  float engine_rpm = 0.0f;
  float crowd_excitement = 0.0f;
  enum Id : int8_t { kNone = -1, kDistance = -2 };
  // The index of the parameter called `name` (kDistance for "(distance)",
  // kNone for one the engine has no source for), and its value.
  static int8_t id_of(std::string_view name);
  float value(int8_t id) const;
};

struct AcousticsSample {
  static constexpr int kMaxActiveEmitters = 8;
  ReverbTemplate reverb{};
  int reverb_template_idx = 0;
  bool in_tunnel = false;
  int ambience_template_idx = 0;
  std::string ambience_snapshot = "DefaultLocation";
  float festival_proximity = 0.0f;  // 0..1 proximity to festival crowd/stage emitters
  float crowd_proximity = 0.0f;     // 0..1 proximity to crowd emitters
  bool is_rally_world = false;      // true when active world is Horizon Rally (ColoradoDirt)
  std::array<RtAmbientEmitter, kMaxActiveEmitters> active_emitters{};
};

// Trivially-copyable, zero-allocation RT acoustics handoff struct for
// TripleBuffer transfer from the main thread to the audio render thread.
struct RtReverbParams {
  float decay_time_s = 1.2f;
  float decay_hf_ratio = 1.0f;
  float density_pct = 100.0f;
  float diffusion_pct = 100.0f;
  float dry_level_mb = 0.0f;
  float room_level_mb = -800.0f;
  float room_hf_level_mb = -1500.0f;
  float reflections_level_mb = -3000.0f;
  float reflections_delay_s = 0.0f;
  float reverb_level_mb = 0.0f;
  float reverb_delay_s = 0.0f;
  float hf_reference_hz = 6260.0f;
  float near_dry = 1.0f;
  float near_wet = 0.0f;
  float far_dry = 0.8f;
  float far_wet = 0.8f;

  static RtReverbParams from_template(const ReverbTemplate& t);
};

struct RtAcousticsSample {
  static constexpr int kMaxActiveEmitters = AcousticsSample::kMaxActiveEmitters;
  RtReverbParams reverb{};
  int reverb_template_idx = 0;
  bool in_tunnel = false;
  int ambience_template_idx = 0;
  std::array<char, 48> ambience_snapshot{};
  float festival_proximity = 0.0f;
  float crowd_proximity = 0.0f;
  bool is_rally_world = false;
  std::array<RtAmbientEmitter, kMaxActiveEmitters> active_emitters{};

  static RtAcousticsSample from_sample(const AcousticsSample& s);
  std::string_view ambience_snapshot_view() const;
};

bool parse_reverb_xml(std::string_view xml, std::vector<ReverbTemplate>& out, std::string* error = nullptr);
bool parse_ambience_xml(std::string_view xml, std::vector<AmbienceTemplate>& out, std::string* error = nullptr);
bool parse_ribbon_audio_xml(std::string_view xml, std::vector<AmbientEmitter3D>& out, std::string* error = nullptr);
bool parse_soundscape_xml(std::string_view xml, SoundscapeTile& out, std::string* error = nullptr);

// Spatial database of reverb templates, ambience templates, festival emitters,
// and loaded .soundscape tiles.
class WorldAcousticsData {
 public:
  bool load_configs(const std::filesystem::path& game_root, std::string* note = nullptr);
  // Loads Horizon Rally (ColoradoDirt) reverb/ambience configs when present under
  // a Rally package dir, direct track dir, or launcher `state_root`.
  bool load_rally_configs(const std::filesystem::path& rally_root_or_state_root, std::string* note = nullptr);
  void set_rally_world(bool on) { rally_world_ = on; }
  bool is_rally_world() const { return rally_world_; }
  // Loads a bounded subset (or all) of .soundscape tiles from Colorado's
  // `media/tracks/colorado/bin.zip` or Rally's `ColoradoDirt_pri_65.zip`.
  int load_soundscapes_from_archive(const std::filesystem::path& bin_zip, int max_tiles = 0);
  void clear_soundscapes();
  void add_tile(SoundscapeTile tile);

  const std::vector<ReverbTemplate>& reverb_templates() const { return reverb_templates_; }
  const std::vector<AmbienceTemplate>& ambience_templates() const { return ambience_templates_; }
  const std::vector<AmbientEmitter3D>& emitters() const { return emitters_; }
  size_t reverb_zone_count() const { return reverb_zones_.size(); }
  size_t ambience_zone_count() const { return ambience_zones_.size(); }

  void set_reverb_templates(std::vector<ReverbTemplate> t) { reverb_templates_ = std::move(t); }
  void set_ambience_templates(std::vector<AmbienceTemplate> t) { ambience_templates_ = std::move(t); }
  void add_emitter(AmbientEmitter3D e);
  // The disc's ambience events by ambience_event_hash; placements then pool
  // on their event's own reach, and those it cannot play stay out of the
  // pool. Empty: every placement pools on its max_distance.
  void set_event_reach(std::vector<std::pair<uint32_t, AmbienceEventReach>> reach);
  const AmbienceEventReach* event_reach(uint32_t hash) const;

  // Evaluates the active blended I3DL2 reverb template, tunnel snapshot flag,
  // ambience zone, festival proximity, and top-4 distance-culled 3D ambient
  // emitters at world position (x, y, z) and optional listener heading.
  AcousticsSample evaluate(float x, float y, float z, float listener_heading_rad = 0.0f) const;

 private:
  bool rally_world_ = false;
  std::vector<ReverbTemplate> reverb_templates_;
  std::vector<AmbienceTemplate> ambience_templates_;
  std::vector<ReverbZone> reverb_zones_;
  std::vector<AmbienceZone> ambience_zones_;
  std::vector<AmbientEmitter3D> emitters_;
  std::vector<std::pair<uint32_t, AmbienceEventReach>> event_reach_;  // sorted by hash
  void apply_reach(AmbientEmitter3D& e) const;
};

// Reads each event's reach from the ambience FEVs AmbienceBedPlayer loads.
std::vector<std::pair<uint32_t, AmbienceEventReach>> load_ambience_event_reach(const std::filesystem::path& game_root);

// Lightweight real-time stereo algorithmic environmental reverb parameterized
// by I3DL2 ReverbTemplate (early slap-back reflections driven by
// reflections_level_mb + 4 stereo HF-damped combs + 2 allpasses + pre-delay).
class EnvironmentalReverb {
 public:
  void prepare(int rate);
  void set_parameters(const ReverbTemplate& tmpl);
  void set_parameters(const RtReverbParams& tmpl);
  void reset();
  // Processes `frames` interleaved stereo frames in place (blends dry + wet).
  void process(float* io, int frames);
  // Renders only the wet reverb return (early reflections + late tail) from
  // `in_stereo` into `out_wet_stereo` (interleaved stereo, overwritten).
  // Returns false when reverb is silent.
  bool process_wet(const float* in_stereo, float* out_wet_stereo, int frames);
  float wet_gain() const { return wet_smooth_; }
  float dry_gain() const { return dry_smooth_; }
  float early_gain() const { return early_smooth_; }
  float target_early_gain() const { return target_early_; }
  float decay_time() const { return decay_time_; }

  // Unity-gain Schroeder allpass filter (exposed for direct unit verification).
  struct Allpass {
    std::vector<float> buf;
    size_t idx = 0;
    float feedback = 0.5f;
    float tick(float in);
  };

 private:
  struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float filter_store = 0.0f;
    float feedback = 0.7f;
    float damp = 0.3f;
    float tick(float in);
  };

  int rate_ = 48000;
  float decay_time_ = 1.2f;
  float target_wet_ = 0.0f;
  float target_dry_ = 1.0f;
  float target_early_ = 0.0f;
  float wet_smooth_ = 0.0f;
  float dry_smooth_ = 1.0f;
  float early_smooth_ = 0.0f;
  size_t predelay_samples_ = 0;
  std::vector<float> predelay_l_, predelay_r_;
  size_t predelay_idx_ = 0;
  std::vector<float> early_l_, early_r_;
  size_t early_idx_ = 0;
  std::array<size_t, 4> early_taps_l_{};
  std::array<size_t, 4> early_taps_r_{};
  std::array<Comb, 4> comb_l_, comb_r_;
  std::array<Allpass, 2> ap_l_, ap_r_;
};

enum class AmbienceBedSlot : int {
  kDefault = 0,
  kMainTown,
  kMountains,
  kRedRock,
  kFestival,
  kCrowd,
  kCount,
};

// Renders regional & festival ambient soundscape beds onto Bus::kAmbience
// based on the active AcousticsSample and vehicle speed. Decodes real
// AMB_*.fev / Colorado_Festival.fev loops asynchronously from disc when
// available, with a gentle filtered procedural fallback when absent.
class AmbienceBedPlayer {
 public:
  static constexpr int kMaxRallyRegions = 13;

  explicit AmbienceBedPlayer(int rate = 48000);
  ~AmbienceBedPlayer();

  // Scans AMB_Quads.fev, AMB_Default.fev, AMB_Festival_Ambience.fev, and
  // Colorado_Festival.fev under `game_root` and decodes the region and crowd
  // beds asynchronously into `cache_dir` (never blocking the audio thread).
  // An event's own loops are decoded once want_events names it.
  int load_from_disc(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir);
  // Queues the loops of these events (ambience_event_hash; the map's
  // placements) for decoding; control thread. Returns how many loops the
  // events wanted so far have.
  int want_events(const std::vector<uint32_t>& event_hashes);
  // Scans up to 13 regional `ColoradoDirt/AMB_*.fev/.fsb` banks from a Rally
  // package dir, direct ColoradoDirt dir, or launcher `state_root` and decodes
  // them asynchronously into `cache_dir`.
  int load_rally_from_disc(const std::filesystem::path& rally_root_or_state_root,
                           const std::filesystem::path& cache_dir);
  // Selects ColoradoDirt Rally regional ambience banks vs base Colorado beds.
  void set_rally_world(bool on) { rally_world_.store(on, std::memory_order_release); }
  bool rally_world() const { return rally_world_.load(std::memory_order_acquire); }
  // Injects a mono PCM loop directly (for unit tests without disc files).
  void set_bed_sample(AmbienceBedSlot slot, PcmSample pcm);
  void set_rally_bed_sample(int region_idx, std::string_view region_name, PcmSample pcm);
  int loaded_bed_count() const;
  // The disc ambience events the player drives (0 until load_from_disc found
  // AMB_*.fev / Colorado_Festival.fev), and their decoded loop beds.
  int loaded_event_count() const;
  int loaded_event_sample_count() const;
  size_t loaded_event_sample_bytes() const { return event_sample_bytes_.load(std::memory_order_acquire); }
  int rally_bed_count() const;
  int last_rally_region() const { return last_rally_region_.load(std::memory_order_acquire); }
  // Frees any replaced bed PCM buffers retired by the audio thread (control thread only).
  void reclaim_retired();

  // With the disc's events: the region bed's gain, every emitter's sound and
  // its rolloff come from the events' parameter curves at `params`.
  void render(const RtAcousticsSample& sample, const AmbienceParams& params, float* out_stereo, int frames);
  void render(const AcousticsSample& sample, float speed_mph, float* out_stereo, int frames,
              float tod_hour = 12.0f);
  void render(const RtAcousticsSample& sample, float speed_mph, float* out_stereo, int frames,
              float tod_hour = 12.0f);

 private:
  // The FEV events behind the beds and emitters, built on the control thread
  // and immutable once published (kept until destruction).
  struct EventTable {
    struct Ref {
      uint32_t hash = 0;
      uint16_t proj = 0, ev = 0;
      std::array<int8_t, 12> param_id{};
    };
    struct SlotRef {
      int proj = -1, ev = -1, layer = 0, inst = 0;
    };
    std::vector<FevProject> projects;
    std::vector<Ref> by_name;  // sorted by hash
    std::vector<int> sd_base;  // per project: the first event-sample id of its sound defs
    std::array<SlotRef, size_t(AmbienceBedSlot::kCount)> slots{};
    std::array<std::array<int8_t, 12>, size_t(AmbienceBedSlot::kCount)> slot_param_id{};
    int sample_count = 0;
    std::vector<bool> has_loop;  // per event-sample id: a decode job exists
    const Ref* find(uint32_t hash) const;
  };
  static constexpr int kMaxEventSamples = 256;
  static constexpr int kMaxEventLevels = 8;
  struct LoadedEventPcm {
    int id = -1;
    PcmSample* pcm = nullptr;
  };
  float event_level(const EventTable& t, int proj, const FevEvent& ev, const std::array<int8_t, 12>& pid,
                    const AmbienceParams& params, float distance, FevInstanceLevel* out, int& count) const;
  struct LoadedBed {
    AmbienceBedSlot slot = AmbienceBedSlot::kDefault;
    PcmSample* pcm = nullptr;
  };
  struct LoadedRallyBed {
    int region_idx = 0;
    std::array<char, 64> name{};
    PcmSample* pcm = nullptr;
  };
  struct EmitterVoice3D {
    uint32_t id_hash = 0;
    float gain = 0.0f;
    float pan = 0.0f;
    double phase = 0.0;
    float lp = 0.0f;
    float bp = 0.0f;
    uint32_t rng = 0x9E3779B9u;
    std::array<double, kMaxEventLevels> ev_phase{};
    std::array<float, kMaxEventLevels> ev_gain{};
    RtAmbientEmitter last{};  // the emitter it last played (faded out from when dropped)
  };
  void stop_worker();
  void stop_rally_worker();
  void retire_pcm(PcmSample* pcm);
  void drain_loaded();
  void render_3d_emitters(const RtAcousticsSample& sample, float* out_stereo, int frames);
  bool render_event_emitter(const EventTable& t, const RtAmbientEmitter& em, EmitterVoice3D& v,
                            const AmbienceParams& params, float* out_stereo, int frames, bool fade_out = false);

  int rate_ = 48000;
  std::array<PcmSample*, int(AmbienceBedSlot::kCount)> beds_{};
  std::array<double, int(AmbienceBedSlot::kCount)> bed_phase_{};
  std::array<float, int(AmbienceBedSlot::kCount)> bed_gain_{};
  // Separate SPSC queues per producer so worker_ and set_bed_sample() never race.
  SpscQueue<LoadedBed> loaded_q_{32};
  SpscQueue<LoadedEventPcm> event_loaded_q_{512};
  std::vector<std::unique_ptr<EventTable>> event_tables_;  // control thread (lifecycle_mu_)
  std::atomic<const EventTable*> events_{nullptr};
  std::array<PcmSample*, kMaxEventSamples> event_pcm_{};
  std::atomic<int> event_sample_count_{0};
  std::atomic<size_t> event_sample_bytes_{0};
  std::array<std::atomic<bool>, kMaxEventSamples> event_wanted_{};  // control thread sets, worker reads
  AmbienceParams params_{};  // audio thread: the parameters of the render in progress
  SpscQueue<LoadedBed> direct_loaded_q_{32};
  SpscQueue<PcmSample*> retired_pcms_{64};
  std::mutex direct_q_mu_;
  std::mutex lifecycle_mu_;  // guards control-thread worker start/stop only (never locked on audio thread)
  std::atomic<bool> stop_worker_{false};
  std::thread worker_;

  std::atomic<bool> rally_world_{false};
  std::atomic<int> last_rally_region_{-1};
  std::array<PcmSample*, kMaxRallyRegions> rally_beds_{};
  std::array<std::array<char, 64>, kMaxRallyRegions> rally_names_{};
  std::array<double, kMaxRallyRegions> rally_phase_{};
  std::array<float, kMaxRallyRegions> rally_gain_{};
  SpscQueue<LoadedRallyBed> rally_loaded_q_{32};
  SpscQueue<LoadedRallyBed> rally_direct_q_{32};
  std::atomic<bool> stop_rally_worker_{false};
  std::thread rally_worker_;
  // Twice the pool: with the disc's events a voice follows its emitter by
  // id, and one the pool dropped fades out while a new one fades in.
  std::array<EmitterVoice3D, 2 * RtAcousticsSample::kMaxActiveEmitters> emitter_voices_{};

  uint32_t rng_ = 0x12345678u;
  float lp_l_ = 0.0f, lp_r_ = 0.0f;
  float bp_l_ = 0.0f, bp_r_ = 0.0f;
  float env_speed_ = 1.0f;
  float env_festival_ = 0.0f;
  double phase_ = 0.0;
};

}  // namespace audio
