// Event configuration tables and 3D spatial/rolloff/Doppler rules:
//   - TiresConfig (Tires.xml + SurfaceChanges.xml)
//   - CollisionAudioConfig (CollisionData.xml)
//   - UiAudioMap (ui4audio.xml, duplicate cue first-wins + ggroup tolerance)
//   - BackfireConfig (BackfireTimerConfig.xml + BackfireVolumeConfig.xml)
//   - RolloffConfig (GlobalRolloffs.xml + GlobalRolloffsTraffic.xml, 6-point
//     rolloff curve, replay FOV scaling, 3D clamps & Doppler)
//   - select_bov_variant (TurboPSI thresholds 4.5 / 13.0)
// Spec: SPEC-AUDIO-EVENTS Parts 2 and 3.
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "audio/engine_sound.h"

namespace audio {

struct TireSurfaceEntry {
  std::string physics_surface;
  std::string lod1_group;
  std::string lod2_group;
  int surface_index = 0;
};

struct SurfaceChangePair {
  std::string s1, s2;
  std::string s1_to_s2;
  std::string s2_to_s1;
};

struct TiresConfig {
  std::vector<TireSurfaceEntry> surfaces;
  std::vector<SurfaceChangePair> changes;

  bool parse_tires(std::string_view xml, std::string* error = nullptr);
  bool parse_surface_changes(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr);

  const TireSurfaceEntry* find_surface(std::string_view physics_surface) const;
  // Returns the event name (e.g. "HardToHard", "DirtToHard", "Silent", or empty).
  std::string surface_change_event(std::string_view from_surface, std::string_view to_surface) const;
};

struct CollisionEventSpec {
  std::string surface1, surface2;
  bool is_break = false;
  std::string impact3d;   // "<Group>/<Event>"
  std::string impact_lfe; // "<Group>/<Event>"
  std::string scrape3d;   // "<Group>/<Event>"
  std::string whoosh3d;   // "<Group>/<Event>"
};

struct CollisionAudioConfig {
  double ms_between_diff_collisions = 75.0;
  std::vector<CollisionEventSpec> pairs;

  bool parse(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr);

  const CollisionEventSpec* find_pair(std::string_view s1, std::string_view s2) const;
};

struct UiAudioEntry {
  std::string play;
  std::string group;
  std::string cue;
  bool is_looping = false;
  bool is_blank() const;
  std::string full_path() const;
};

struct UiAudioMap {
  std::vector<UiAudioEntry> defaults;
  std::map<std::string, std::vector<UiAudioEntry>> overrides;  // lower-case override name -> entries
  std::map<std::string, std::string> scene_overrides;          // lower-case scene name -> lower-case override name

  bool parse(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr);

  int total_entries() const;
  const UiAudioEntry* resolve(std::string_view play_name, std::string_view scene_name = {}) const;
};

struct BackfireLevelTiming {
  double min_time_ms = 0, max_time_ms = 0;
  double min_norm_rpm = 0, max_norm_rpm = 0;
  double silence_weighting = 100;
  Param volume;
};

struct BackfireConfig {
  std::array<BackfireLevelTiming, 4> levels{};

  bool parse_timer(std::string_view xml, std::string* error = nullptr);
  bool parse_volume(std::string_view xml, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr);
};

// 6-point piecewise-linear 3D rolloff curve (SPEC-AUDIO-EVENTS Part 3).
struct RolloffCurve {
  float min_dist = 10.0f;
  float max_dist = 30.0f;
  float begin_fade_to_zero_dist = 70.0f;
  float fade_to_zero_dist = 100.0f;

  float eval(float distance_m, float fov_scale = 1.0f) const;
};

struct RolloffConfig {
  RolloffCurve intake_chuff{};
  RolloffCurve burble{};
  RolloffCurve backfire{};
  RolloffCurve chassis_intake{};
  RolloffCurve chassis_ambient{};
  RolloffCurve chassis_exhaust_l{};
  RolloffCurve chassis_exhaust_r{};

  bool parse(std::string_view xml, bool replay = false, std::string* error = nullptr);
  bool load(const std::filesystem::path& game_root, bool traffic = false, bool replay = false,
            std::string* error = nullptr);
};

// Replay camera FOV zoom scaling (SPEC-AUDIO-EVENTS Part 3).
float replay_fov_rolloff_scale(float fov_rad, float fov_ref_rad = 0.872665f, float listener_scalar = 0.45f);

// Clamped 3D vector helper (position [-20000, 20000] m, velocity [-300, 300] m/s)
// and Doppler pitch factor (c = 340 m/s).
struct Vec3f {
  float x = 0, y = 0, z = 0;
};

Vec3f clamp_audio_position(Vec3f p);
Vec3f clamp_audio_velocity(Vec3f v);
float compute_doppler_pitch(Vec3f listener_pos, Vec3f listener_vel, Vec3f emitter_pos, Vec3f emitter_vel,
                            float doppler_factor = 1.0f);
// Computes equal-power stereo pan (L, R) from listener forward/right axes and emitter direction.
void compute_stereo_pan(Vec3f listener_pos, Vec3f listener_right, Vec3f emitter_pos, float& left, float& right);

// Confirmed Turbo BOV threshold rule (SPEC-AUDIO-EVENTS Part 2).
enum class BovVariant : uint8_t { kNone = 0, kLo, kHi };
BovVariant select_bov_variant(double turbo_psi);

}  // namespace audio
