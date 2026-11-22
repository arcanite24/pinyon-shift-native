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
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
};

struct SoundscapeTile {
  std::vector<ReverbZone> reverb_zones;
  std::vector<AmbienceZone> ambience_zones;
  std::vector<AmbientEmitter3D> emitters;
  int tree_count = 0;
  int crowd_count = 0;
};

struct AcousticsSample {
  ReverbTemplate reverb{};
  int reverb_template_idx = 0;
  bool in_tunnel = false;
  int ambience_template_idx = 0;
  std::string ambience_snapshot = "DefaultLocation";
  float festival_proximity = 0.0f;  // 0..1 proximity to festival crowd/stage emitters
  float crowd_proximity = 0.0f;     // 0..1 proximity to crowd emitters
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
  // Loads a bounded subset (or all) of .soundscape tiles from Colorado's
  // `media/tracks/colorado/bin.zip` into the spatial index.
  int load_soundscapes_from_archive(const std::filesystem::path& bin_zip, int max_tiles = 0);
  void add_tile(SoundscapeTile tile);

  const std::vector<ReverbTemplate>& reverb_templates() const { return reverb_templates_; }
  const std::vector<AmbienceTemplate>& ambience_templates() const { return ambience_templates_; }
  const std::vector<AmbientEmitter3D>& emitters() const { return emitters_; }
  size_t reverb_zone_count() const { return reverb_zones_.size(); }
  size_t ambience_zone_count() const { return ambience_zones_.size(); }

  void set_reverb_templates(std::vector<ReverbTemplate> t) { reverb_templates_ = std::move(t); }
  void set_ambience_templates(std::vector<AmbienceTemplate> t) { ambience_templates_ = std::move(t); }
  void add_emitter(AmbientEmitter3D e) { emitters_.push_back(std::move(e)); }

  // Evaluates the active blended I3DL2 reverb template, tunnel snapshot flag,
  // ambience zone, and festival proximity at world position (x, y, z).
  AcousticsSample evaluate(float x, float y, float z) const;

 private:
  std::vector<ReverbTemplate> reverb_templates_;
  std::vector<AmbienceTemplate> ambience_templates_;
  std::vector<ReverbZone> reverb_zones_;
  std::vector<AmbienceZone> ambience_zones_;
  std::vector<AmbientEmitter3D> emitters_;
};

// Lightweight real-time stereo algorithmic environmental reverb parameterized
// by I3DL2 ReverbTemplate (4 stereo HF-damped combs + 2 allpasses + pre-delay).
class EnvironmentalReverb {
 public:
  void prepare(int rate);
  void set_parameters(const ReverbTemplate& tmpl);
  void reset();
  // Processes `frames` interleaved stereo frames in place (blends dry + wet).
  void process(float* io, int frames);
  float wet_gain() const { return wet_smooth_; }
  float decay_time() const { return decay_time_; }

 private:
  struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float filter_store = 0.0f;
    float feedback = 0.7f;
    float damp = 0.3f;
    float tick(float in);
  };
  struct Allpass {
    std::vector<float> buf;
    size_t idx = 0;
    float feedback = 0.5f;
    float tick(float in);
  };

  int rate_ = 48000;
  float decay_time_ = 1.2f;
  float target_wet_ = 0.0f;
  float target_dry_ = 1.0f;
  float wet_smooth_ = 0.0f;
  float dry_smooth_ = 1.0f;
  size_t predelay_samples_ = 0;
  std::vector<float> predelay_l_, predelay_r_;
  size_t predelay_idx_ = 0;
  std::array<Comb, 4> comb_l_, comb_r_;
  std::array<Allpass, 2> ap_l_, ap_r_;
};

// Renders regional & festival ambient soundscape beds onto Bus::kAmbience
// based on the active AcousticsSample and vehicle speed.
class AmbienceBedPlayer {
 public:
  explicit AmbienceBedPlayer(int rate = 48000);
  void render(const AcousticsSample& sample, float speed_mph, float* out_stereo, int frames);

 private:
  int rate_ = 48000;
  uint32_t rng_ = 0x12345678u;
  float lp_l_ = 0.0f, lp_r_ = 0.0f;
  float bp_l_ = 0.0f, bp_r_ = 0.0f;
  float env_speed_ = 1.0f;
  float env_festival_ = 0.0f;
  double phase_ = 0.0;
};

}  // namespace audio
