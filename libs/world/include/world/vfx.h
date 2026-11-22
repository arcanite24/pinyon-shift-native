// Particle effects, world ambient emitters, exhaust backfire flames, and
// persistent/fading tyre skidmarks & ruts (SPEC-VFX-AND-PARTICLES).
//
// Reads the player's media/effects.zip (261 XMLs + 72 .xds textures),
// Ribbon_00/ParticleEmitters.xml (2,367 entries), Ribbon_00/SmashableEffects.xml,
// and audio/BackfireTimerConfig.xml. All simulation positions and velocities
// are in the game frame (+X right, +Y up, +Z forward/north).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fh1data/collision.h"
#include "fh1data/xds.h"

namespace fh1 {
class Archive;
}

namespace world {

enum class ParticleShaderKind : uint8_t {
  kDefault = 0,
  kSoft = 1,
  kAnimatedSoft = 2,
  kAnimatedRowOnly = 3,
  kAnimatedRowOnlyGradient = 4,
};

struct ParticleEffectDef {
  std::string name;  // lower-case stem, e.g. "smoke", "dust1", "backfire"
  int version = 14;
  int budget = 120;
  int lower_budget = 60;

  // <TimeOfDayData startH startM endH endM>
  int start_h = 0, start_m = 0;
  int end_h = 24, end_m = 0;

  // <EmitterProperties>
  float off_axis_deg = 0.0f, off_axis_var_deg = 10.0f;
  float off_plane_deg = 0.0f, off_plane_var_deg = 10.0f;
  float quantity = 20.0f, quantity_var = 0.0f;
  float velocity = 1.0f, velocity_var = 0.25f;
  bool emit_infinite = true;
  float life = 1.5f, life_var = 0.25f;
  float density = 1.0f;
  ParticleShaderKind shader_kind = ParticleShaderKind::kSoft;
  float anim_rate = 1.0f;
  int num_frames_x = 1, num_frames_y = 1;
  bool render_in_mirror = true;

  // <Size>
  float size_x = 0.25f, size_y = 0.25f;
  float size_var_x = 0.1f, size_var_y = 0.1f;
  float grow_for_x = 0.5f, grow_for_y = 0.5f;
  float shrink_for_x = 0.0f, shrink_for_y = 0.0f;
  float max_size_x = 1.0f, max_size_y = 1.0f;
  float end_size_x = 0.0f, end_size_y = 0.0f;
  float grow_from = 0.0f, shrink_to = 1.0f;

  // <SpinTime>, <SpinPhase>
  float spin_time = 0.0f, spin_time_var = 0.0f;
  float spin_phase = 0.0f, spin_phase_var = 0.0f;
  bool spin_alternate = false;

  // <Behavior>
  float motion_influence = 0.0f;  // 0..100
  float motion_multiplier = 0.0f;
  float gravity_strength = 0.0f;  // negative lifts smoke, positive drops dirt/sparks
  float gravity_decay = 0.0f;
  float friction_strength = 0.0f;
  float friction_vel_kept = 1.0f;
  std::string child_spawn;  // <ParticleSpawn type="..."/>

  // <Render><ParticleType>
  bool velocity_quad = false;  // Quad vs BillboardedQuad
  bool dynamic_lighting = true;
  bool zsort = false;
  float max_screen_size = 0.5f;
  std::array<float, 4> light_influence{0.25f, 1.5f, 1.5f, 1.0f};
  float headlight_power = 1.0f;
  std::string texture_name;  // lower-case .xds filename in effects.zip

  // <Material>
  bool additive = false;
  std::array<float, 4> rgb_start{1.0f, 1.0f, 1.0f, 0.6f};
  std::array<float, 4> rgb_mid{1.0f, 1.0f, 1.0f, 0.4f};
  std::array<float, 4> rgb_end{0.4f, 0.4f, 0.4f, 0.0f};
  float fade_in_to = 0.15f;
  float fade_out_from = 0.55f;
  float near_fade = 0.0f;

  bool active_at_hour(float hour) const;
};

struct BackfireTier {
  float min_time_ms = 0.0f;
  float max_time_ms = 0.0f;
  float min_norm_rpm = 0.0f;
  float max_norm_rpm = 0.0f;
  float silence_weighting = 100.0f;
};

struct BackfireTimerConfig {
  std::array<BackfireTier, 4> tiers{};
  bool loaded = false;
};

enum class WorldEmitterKind : uint8_t {
  kSimple = 0,
  kTriggerZone = 1,
  kTriggerable = 2,
};

enum class WorldEmitterVolume : uint8_t {
  kSuperspray = 0,
  kSphere = 1,
  kPlane = 2,
  kBox = 3,
};

struct WorldEmitter {
  WorldEmitterKind kind = WorldEmitterKind::kSimple;
  WorldEmitterVolume volume = WorldEmitterVolume::kSuperspray;
  std::string name;
  std::string gameplay_id;
  std::string effect;       // raw effect name (or linked emitter name for TriggerZone)
  std::array<float, 3> pos{};  // game frame (file z negated)
  std::array<float, 3> x_axis{1, 0, 0};
  std::array<float, 3> y_axis{0, 1, 0};
  std::array<float, 3> z_axis{0, 0, 1};
  float radius = 1.0f;
  float width = 1.0f, length = 1.0f, height = 1.0f;
  float off_axis_spread = 0.0f, plane_spread = 0.0f;
  float retrigger_delay = 1.0f;
};

// Standalone XML parsers (used by both ParticleLibrary and unit tests).
bool parse_particle_effect_xml(std::string_view xml, const std::string& name, ParticleEffectDef& out);
bool parse_smashable_effects_xml(std::string_view xml, std::map<std::string, std::string>& out);
bool parse_backfire_timer_config_xml(std::string_view xml, BackfireTimerConfig& out);
bool parse_particle_emitters_xml(std::string_view xml, std::vector<WorldEmitter>& out);

// Expands composite world emitter names (e.g. "_Composite_MainStage",
// "_Composite_Flare_1", "_Composite_CampFire") into concrete effects.zip stems.
std::vector<std::string> expand_composite_effect(std::string_view effect_name);

class ParticleLibrary {
 public:
  ParticleLibrary();
  ~ParticleLibrary();

  // Loads effects.zip, Ribbon_00/SmashableEffects.xml, Ribbon_00/ParticleEmitters.xml,
  // and audio/BackfireTimerConfig.xml. Missing optional files degrade gracefully.
  bool load(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
            std::string* error = nullptr);

  const ParticleEffectDef* find_effect(std::string_view name) const;
  const std::map<std::string, ParticleEffectDef>& effects() const { return effects_; }
  size_t xds_count() const { return xds_members_.size(); }
  const std::map<std::string, std::string>& smashable_effects() const { return smashable_effects_; }
  std::string smashable_effect_for(std::string_view stem) const;
  const std::vector<WorldEmitter>& world_emitters() const { return world_emitters_; }
  const BackfireTimerConfig& backfire_config() const { return backfire_config_; }

  // Decodes a named .xds texture from media/effects.zip (case-insensitive).
  bool decode_texture(std::string_view filename, fh1::RgbaImage& out) const;
  // Registers a synthetic effect (for unit tests without disc files).
  void add_effect(ParticleEffectDef def);

 private:
  std::unique_ptr<fh1::Archive> effects_zip_;
  std::map<std::string, ParticleEffectDef> effects_;
  std::unordered_map<std::string, std::string> xds_members_;  // lower-case name -> archive member
  std::map<std::string, std::string> smashable_effects_;      // lower-case stem -> effect name
  std::vector<WorldEmitter> world_emitters_;
  BackfireTimerConfig backfire_config_;
};

// Render-ready particle sprite in game-frame world coordinates.
struct ParticleSprite {
  std::array<float, 3> pos{};
  std::array<float, 3> vel{};  // used for velocity-aligned quads (sparks, backfire)
  float size_x = 0.25f;
  float size_y = 0.25f;
  float spin_rad = 0.0f;
  float soft_depth_m = 0.5f;   // 0 = hard, >0 = linear depth-fade thickness (m)
  std::array<float, 4> rgba{1, 1, 1, 1};
  std::array<float, 4> light_influence{0.25f, 1.5f, 1.5f, 1.0f};
  float headlight_power = 1.0f;
  float max_screen_size = 0.5f;
  int frame_index = 0;
  int frames_x = 1;
  int frames_y = 1;
  uint16_t texture_slot = 0;   // assigned by texture mapper
  bool additive = false;
  bool velocity_aligned = false;
  bool dynamic_lighting = true;
};

// Per-wheel state supplied each frame for tyre smoke/dust/gravel/ruts.
struct WheelVfxInput {
  bool grounded = false;
  std::array<float, 3> contact_pos{};     // game frame
  std::array<float, 3> surface_normal{0, 1, 0};
  std::array<float, 3> wheel_right{1, 0, 0};
  std::array<float, 3> wheel_forward{0, 0, 1};
  float tyre_width_m = 0.24f;
  float slip_combined = 0.0f;             // combined normalized slip magnitude
  float wheel_speed_mps = 0.0f;           // |omega * radius|
  int surface_index = 0;
  fh1::SurfaceType surface{};
};

struct VehicleVfxInput {
  std::array<float, 3> car_pos{};         // game frame
  std::array<float, 3> car_vel{};         // game frame (m/s)
  std::array<float, 3> car_forward{0, 0, 1};
  std::array<float, 3> car_right{1, 0, 0};
  std::array<float, 3> car_up{0, 1, 0};
  std::array<WheelVfxInput, 4> wheels{};
  std::array<std::array<float, 3>, 2> exhaust_tips{};  // game frame
  int exhaust_tip_count = 0;
  float norm_rpm = 0.0f;
  float throttle = 0.0f;
  bool upshift_event = false;
  int backfire_level = 0;                 // 0 stock, 1..3 street/sport/race
  uint32_t audio_backfire_pulses = 0;     // coupled to Slice 2 SfxEngine backfire pops
  bool scraping = false;
  std::array<float, 3> scrape_pos{};
  std::array<float, 3> scrape_normal{0, 1, 0};
  bool scrape_should_spark = false;
};

class ParticleSystem {
 public:
  static constexpr size_t kMaxParticles = 2400;

  ParticleSystem() = default;
  void set_library(const ParticleLibrary* lib);
  void clear();

  // Assigns a texture slot index to a lower-case .xds texture name.
  uint16_t resolve_texture_slot(std::string_view tex_name);
  const std::vector<std::string>& used_textures() const { return used_textures_; }

  // Emits a one-shot burst (backfire flame, crash sparks, smashable BI_*, fireworks).
  void spawn_burst(std::string_view effect_name, const std::array<float, 3>& pos,
                   const std::array<float, 3>& emit_dir, const std::array<float, 3>& emitter_vel,
                   int count, float intensity_scale = 1.0f);

  // Emits continuous particles with per-key fractional rate accumulator.
  void emit_continuous(std::string_view effect_name, const std::array<float, 3>& pos,
                       const std::array<float, 3>& emit_dir, const std::array<float, 3>& emitter_vel,
                       float dt, float intensity_scale, uint64_t emitter_key);

  // Updates vehicle wheel smoke/dust/dirt/grass/gravel, sparks, and backfire flames.
  void update_vehicle(const VehicleVfxInput& in, float dt);

  // Spawns collision sparks + smoke burst at an impact point.
  void spawn_collision_impact(const std::array<float, 3>& pos, const std::array<float, 3>& normal,
                              const std::array<float, 3>& vel, float rel_speed_mps, bool should_spark);

  // Spawns the Ribbon_00/SmashableEffects.xml burst for a smashed prop stem.
  void spawn_smashable_burst(std::string_view prop_stem, const std::array<float, 3>& pos,
                             const std::array<float, 3>& impulse_vel);

  // Steps ambient world emitters (leaves, festival confetti, fireworks, flares, vents).
  void update_world_emitters(const std::array<float, 3>& focus_pos, float tod_hour, float dt);

  // Advances all live particles by `dt` seconds with wind and removes expired ones.
  void step(float dt, const std::array<float, 3>& wind_dir = {1.0f, 0.0f, 0.0f}, float wind_speed = 1.45f);

  // Fills render-ready sprite lists split into alpha-blended (back-to-front sorted
  // relative to `eye_game`) and additive batches.
  void build_sprites(const std::array<float, 3>& eye_game, std::vector<ParticleSprite>& alpha_out,
                     std::vector<ParticleSprite>& additive_out) const;

  size_t live_count() const { return particles_.size(); }

 private:
  struct LiveParticle {
    const ParticleEffectDef* def = nullptr;
    uint16_t tex_slot = 0;
    std::array<float, 3> pos{};
    std::array<float, 3> vel{};
    float age = 0.0f;
    float life = 1.0f;
    float size_scale_x = 1.0f;
    float size_scale_y = 1.0f;
    float spin = 0.0f;
    float spin_rate = 0.0f;
    float alpha_scale = 1.0f;
    int anim_row = 0;
  };

  void spawn_one(const ParticleEffectDef& def, const std::array<float, 3>& pos,
                 const std::array<float, 3>& emit_dir, const std::array<float, 3>& emitter_vel,
                 float intensity_scale);
  float rand01();
  float rand_sym();

  const ParticleLibrary* lib_ = nullptr;
  std::vector<LiveParticle> particles_;
  std::unordered_map<uint64_t, float> accumulators_;
  std::unordered_map<std::string, uint16_t> tex_slot_map_;
  std::vector<std::string> used_textures_;
  uint32_t rng_ = 0x51C4BEEFu;
  float fallback_backfire_cooldown_ = 0.0f;
};

// Terrain-conforming skidmark / tyre-rut vertex (game frame, lifted slightly
// along the contact normal).
struct SkidVertex {
  std::array<float, 3> pos{};
  std::array<float, 3> normal{0, 1, 0};
  std::array<float, 2> uv{};
  std::array<uint8_t, 4> rgba{0, 0, 0, 0};
};

// Persistent/fading surface tracks ring buffer (CSkidMarks + surfaceTypes.xml).
class SkidMarkSystem {
 public:
  // Matches CSkidMarks ring-buffer capacity constants (4,000 segments -> 16,000 verts).
  static constexpr size_t kMaxSegments = 4000;
  static constexpr size_t kMaxWheels = 32;  // up to 8 cars * 4 wheels

  SkidMarkSystem();
  void clear();
  void break_wheel(size_t wheel_slot);
  void break_all();

  // Updates one wheel's track strip. `wheel_slot` in [0, kMaxWheels).
  void update_wheel(size_t wheel_slot, const WheelVfxInput& in);

  // Computes combined slip intensity in [0, 1] per SPEC-VFX-AND-PARTICLES.
  static float slip_intensity(float slip_combined, float min_intensity, float wheel_speed_mps);

  // Exports triangle vertices for texture_index 0 (asphalt rubber) and 1 (dirt/gravel ruts),
  // applying smooth FIFO tail fade to the oldest segments.
  void build_geometry(std::vector<SkidVertex>& asphalt_tris, std::vector<SkidVertex>& rut_tris) const;

  size_t segment_count() const { return count_; }
  size_t count_by_texture(int texture_index) const;

 private:
  struct Segment {
    std::array<SkidVertex, 4> v{};  // 0=prevL, 1=prevR, 2=currR, 3=currL
    int texture_index = 0;
    uint64_t seq = 0;
  };
  struct WheelStrip {
    bool active = false;
    std::array<float, 3> prev_center{};
    std::array<float, 3> prev_left{};
    std::array<float, 3> prev_right{};
    std::array<float, 3> prev_normal{0, 1, 0};
    std::array<uint8_t, 4> prev_rgba{0, 0, 0, 0};
    float v_coord = 0.0f;
    int texture_index = -1;
  };

  std::vector<Segment> ring_;
  size_t head_ = 0;
  size_t count_ = 0;
  uint64_t next_seq_ = 1;
  std::array<WheelStrip, kMaxWheels> strips_{};
};

}  // namespace world
