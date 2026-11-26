// FMOD Ex Event Project (.fev, magic "FEV1") binary parser supporting both
// disc versions 0x00370000 and 0x00400000 down to the trailing "comp" chunk.
// Spec: SPEC-AUDIO-EVENTS Part 1.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace audio {

struct FevWavebank {
  uint32_t bank_type = 0;
  uint32_t max_streams = 0;
  uint64_t hash = 0;
  std::string name;  // resolves to <name>.fsb in the project's folder
};

struct FevCategory {
  std::string name;
  float volume = 1.0f;
  float pitch = 0.0f;
  uint32_t max_playbacks = 0;
  uint32_t max_playbacks_behavior = 0;
  std::vector<FevCategory> subcategories;
};

struct FevSoundInstance {
  uint16_t sound_def_index = 0;
  float start_pos = 0.0f;
  float length = 0.0f;
  uint32_t start_mode = 0;
  uint32_t loop_mode = 0;
  int32_t loop_count = 0;
  uint32_t autopitch_enabled = 0;
  float autopitch_ref = 0.0f;
  float autopitch_min = 0.0f;
  float fine_tune = 0.0f;
  float volume = 1.0f;
  float fadein_len = 0.0f;
  float fadeout_len = 0.0f;
  uint32_t fadein_type = 0;
  uint32_t fadeout_type = 0;
};

struct FevEnvelopePoint {
  float x = 0.0f;
  float y = 0.0f;
  uint32_t curve_shape = 0;
};

struct FevEnvelope {
  int32_t parent_index = -1;
  std::string dsp_name;
  uint32_t dsp_param_index = 0;
  uint32_t flags = 0;
  uint32_t ex_flags = 0;
  std::vector<FevEnvelopePoint> points;
  uint32_t param_index = 0;
  uint32_t mapping_flags = 0;

  // Piecewise-linear evaluation at normalized/raw parameter x.
  float eval(float x) const;
};

struct FevLayer {
  uint16_t signature = 0;
  int16_t priority = 0;
  uint16_t control_param_index = 0;
  std::vector<FevSoundInstance> instances;
  std::vector<FevEnvelope> envelopes;
};

struct FevParameter {
  std::string name;
  float velocity = 0.0f;
  float min_val = 0.0f;
  float max_val = 1.0f;
  uint32_t flags = 0;  // bit 0x02 = primary; bit 0x04 = automatic distance
  float seek_speed = 0.0f;
  uint32_t loop_behavior = 0;
  std::vector<float> sustain_points;
};

struct FevEvent {
  std::string path;  // "<project>/<group>[/<subgroup>...]/<event>"
  std::string name;
  uint32_t event_type = 0;  // 0x08 complex, 0x10 simple
  float volume = 1.0f;
  float pitch = 0.0f;
  float pitch_rand = 0.0f;
  float volume_rand = 0.0f;
  uint32_t priority = 0;
  uint32_t max_playbacks = 0;
  uint32_t steal_priority = 0;
  uint32_t mode = 0;  // bit 0x10 = 3D, bit 0x08 = 2D
  float min_distance = 1.0f;
  float max_distance = 100.0f;
  std::array<float, 6> speaker_level{1, 1, 1, 0, 1, 1};
  float cone_inside_angle = 360.0f;
  float cone_outside_angle = 360.0f;
  float cone_outside_volume = 1.0f;
  uint32_t max_playbacks_behavior = 0;
  float doppler_factor = 1.0f;
  float reverb_dry_level = 0.0f;
  float reverb_wet_level = 0.0f;
  float speaker_spread = 0.0f;
  uint32_t fadein_ms = 0;
  uint32_t fadeout_ms = 0;
  float spawn_intensity = 1.0f;
  float pan_level = 1.0f;
  uint32_t position_rand = 0;

  std::vector<FevSoundInstance> simple_instances;
  std::vector<FevLayer> layers;
  std::vector<FevParameter> params;
  std::vector<std::string> categories;

  bool is_complex() const { return event_type == 0x08u; }
  bool is_simple() const { return event_type == 0x10u; }
  bool is_3d() const { return (mode & 0x10u) != 0; }
  int find_param(std::string_view param_name) const;  // case-insensitive, -1 if none
};

struct FevSoundDefEntry {
  uint32_t entry_type = 0;  // 0 wavetable, 1 oscillator, 2 silence, 3 programmer
  uint32_t weight = 100;
  std::string source_wav;
  std::string wavebank_name;
  uint32_t subsound_index = 0;
  uint32_t length_ms = 0;
  uint32_t osc_type = 0;
  float osc_freq = 0.0f;
};

// The head of a sound-definition property template (section 6). On the
// disc's AMB_Default.fev: u32 play mode, u32 spawn time min and max (ms), u32
// maximum spawned sounds, f32 volume. Spawned one-shots carry a spawn time
// (Tree_Rustle 1,000..10,000 ms, Air_Horns 500..30,000, Car_Door_Slams
// 0..30,000, Shouts_Lots 0..2,000 with 3 spawned); the continuous beds
// (River, Crickets, Race_Crowd_Amb) carry 0..0. Then four f32 (1.0 on every
// Tires.fev template) and, at byte 36, the f32 pitch in the event-pitch unit
// (kFevPitchOctaves; Tires.fev's PeelOut template -0.05, HardtoHard -0.0625).
// The rest of the block (randomisation and recalculation flags) is not decoded.
struct FevSoundDefTemplate {
  uint32_t play_mode = 0;
  uint32_t spawn_min_ms = 0;
  uint32_t spawn_max_ms = 0;
  uint32_t max_spawned = 1;
  float volume = 1.0f;
  float pitch = 0.0f;
  bool spawned() const { return spawn_max_ms > 0; }
};

struct FevSoundDef {
  std::string name;
  uint32_t template_index = 0;
  std::vector<FevSoundDefEntry> entries;
};

struct FevProject {
  uint32_t version = 0;
  std::string name;
  std::filesystem::path dir;
  std::vector<FevWavebank> wavebanks;
  FevCategory root_category;
  std::vector<FevEvent> events;
  std::vector<FevSoundDefTemplate> templates;
  std::vector<FevSoundDef> sound_defs;
  uint32_t reverb_count = 0;

  // The sound definition's property template, or null.
  const FevSoundDefTemplate* template_of(const FevSoundDef& sd) const {
    return sd.template_index < templates.size() ? &templates[sd.template_index] : nullptr;
  }

  // Case-insensitive lookup by full path "<project>/<group>/.../<event>"
  // (or "<group>/.../<event>" without project prefix).
  const FevEvent* find_event(std::string_view path) const;
};

// --- Event evaluation (complex events) --------------------------------------
// What the disc's events show (all 34 projects, dumped):
//  - Envelope x and an instance's start_pos/length are normalised to the
//    parameter's [min_val, max_val] (x <= 1 for Speed 0..275, RPM 0..16000).
//  - An envelope with no DSP name and flag 0x08 is the layer's volume: y is
//    linear amplitude (0.708, 0.501, 0.251 = -3, -6, -12 dB grid points), and
//    a layer's volume envelopes multiply.
//  - Flag 0x10 (no DSP name) is the layer's pitch: y = 0.5 is unity and y
//    spans -4..+4 octaves (kFevPitchEnvOctaves); a layer's pitch envelopes add.
//  - Event pitch and a sound definition template's pitch are in units of
//    4 octaves (kFevPitchOctaves): the recomp plays Tires.fev's PeelOut loops
//    (template -0.05, envelope 0.4896) at 0.82 of their rate, 2^(-0.2 - 0.083).
//  - An instance sounds while the layer's control parameter is inside
//    [start_pos, start_pos + length]; fadein_len / fadeout_len are fractions
//    of the instance's own length (-1 = none). Damage.fev's DamSlow fade-out
//    (0.689 x 0.819) and DamFast fade-in (0.758 x 0.745) both span the two
//    instances' overlap, 0.564. Fades are linear (curve shapes undecoded).
// DSP-unit envelopes (Lowpass, Highpass, Tremolo, Flange, Speaker Level) are
// not evaluated.
inline constexpr uint32_t kFevEnvVolume = 0x08u;
inline constexpr uint32_t kFevEnvPitch = 0x10u;
inline constexpr float kFevPitchOctaves = 4.0f;     // octaves per unit of event / template pitch
inline constexpr float kFevPitchEnvOctaves = 8.0f;  // octaves across a pitch envelope's y 0..1
// An unnamed unit whose parameters are a surround pan: param 0 left/right
// (1 = left), param 1 back/front (1 = front), param 2 LFE send. The disc's
// countdown stems Left / Right / Left Surround / Right Surround carry
// (1,1) / (0,1) / (1,0) / (0,0).
inline constexpr uint32_t kFevEnvSurroundPan = 0x80u;

struct FevInstanceLevel {
  uint16_t layer = 0;
  uint16_t instance = 0;
  uint16_t sound_def = 0;
  float gain = 0.0f;       // event volume x instance volume x crossfade x volume envelopes
  float pitch_env = 0.5f;  // the layer's pitch envelope y (0.5 = unity; scale undecoded)
  bool has_pitch_env = false;
};

// `raw` normalised to the parameter's range and clamped to [0, 1].
float fev_normalize(const FevParameter& p, float raw);
// Evaluates every layer instance of `ev` at the raw parameter values `raw`
// (indexed like ev.params; missing entries read as the parameter's minimum).
// Writes at most `cap` levels (audible or not) and returns how many it wrote.
// No allocation: safe on the audio thread.
int evaluate_fev_event(const FevEvent& ev, const float* raw, size_t raw_count, FevInstanceLevel* out, int cap);

bool parse_fev(std::span<const uint8_t> bytes, FevProject& out, std::string* error = nullptr);
bool load_fev(const std::filesystem::path& path, FevProject& out, std::string* error = nullptr);

}  // namespace audio
