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
  std::vector<FevSoundDef> sound_defs;
  uint32_t reverb_count = 0;

  // Case-insensitive lookup by full path "<project>/<group>/.../<event>"
  // (or "<group>/.../<event>" without project prefix).
  const FevEvent* find_event(std::string_view path) const;
};

bool parse_fev(std::span<const uint8_t> bytes, FevProject& out, std::string* error = nullptr);
bool load_fev(const std::filesystem::path& path, FevProject& out, std::string* error = nullptr);

}  // namespace audio
