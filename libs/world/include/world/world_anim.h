// Animated world props & spectator/grass catalog (SPEC-VFX-AND-PARTICLES):
// - Parses media/Spectators.zip (spectators.xml: sprite angles, models,
//   modelsets, skeletons, crowdclasses) and inventories media/animatedobjects.zip.
// - Drives animated world geometry: hot-air balloons, spinning ranch windmills,
//   cheering festival crowds, waving Horizon festival banner flags, and
//   wind-swaying roadside grass quads (_0x00005007 / grass shell parameters from
//   TrackSettings.xml).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "drive/vehicle.h"
#include "world/smashables.h"
#include "world/tod.h"

namespace world {

struct SpectatorVertex {
  std::array<float, 3> pos{};
  std::array<float, 3> normal{};
  std::array<float, 2> uv{};
  std::array<uint8_t, 4> rgba{255, 255, 255, 255};
  std::array<float, 2> uv2{0.9375f, 0.9375f};  // night stage/outpost lightmap UV into spectator atlas
};

struct SpectatorMeshLod {
  enum BoneRole : uint8_t {
    kBoneLower = 0,
    kBoneTorso = 1,
    kBoneHead = 2,
    kBoneUpperArmL = 3,
    kBoneForearmL = 4,
    kBoneUpperArmR = 5,
    kBoneForearmR = 6,
  };
  static constexpr size_t kMaxBones = 64;
  struct Vertex {
    std::array<float, 3> pos{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    uint8_t bone0 = 0;
    uint8_t bone1 = 0;
    uint8_t weight0 = 255;
    uint8_t weight1 = 0;
  };
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;  // preferred-LOD outward-facing CCW triangles in game frame
  std::array<std::vector<uint16_t>, 3> lod_indices;  // LOD0 (near), LOD1 (mid), LOD2 (far)
  std::array<uint8_t, kMaxBones> bone_role{};
  std::array<std::array<float, 3>, kMaxBones> bone_centroid{};
  float min_y = 0.0f;
  bool has_arm_bones = false;
  bool has_stand_skeleton = false;  // authentic 19-bone FH1 standing rig (bones 0..18)
  bool has_sit_skeleton = false;    // authentic 15-bone FH1 sitting rig (bones 0..14)
};

struct SpectatorAsset {
  std::string name;
  SpectatorMeshLod mesh;
  uint32_t atlas_slot = 0;  // cell index [0..19] in 8x8 RGBA8 atlas
  bool is_male = true;
  bool is_sitting = false;
};

struct SpectatorStandBone {
  int16_t parent = -1;
  std::array<float, 3> bind_world{};
};

struct SpectatorAnimClip {
  std::string name;
  float duration_sec = 3.0f;
  uint16_t bone_count = 19;
  uint16_t frame_count = 90;
  bool is_sitting = false;
  bool is_cheer = false;
  float energy = 0.5f;  // normalized clip cadence/energy modulation [0..1]
};

struct AnimatedPgeoSubmesh {
  struct Vertex {
    std::array<float, 3> pos{};
    std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
  };
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;
  std::array<float, 3> box_min{};
  std::array<float, 3> box_max{};
};

enum class AnimatedPgeoCategory : uint8_t {
  kOther = 0,
  kBalloon = 1,
  kWindmill = 2,
  kBannerOrFlag = 3,
  kAirwalker = 4,
  kStageLight = 5,
  kAircraft = 6,
  kRallyProp = 7,
  kFerrisWheel = 8,
  kTrafficLight = 9,
};

struct AnimatedPgeoObject {
  std::string name;
  uint32_t kind = 0;  // kind 4 for animatedobjects.zip animated meshes
  AnimatedPgeoCategory category = AnimatedPgeoCategory::kOther;
  std::array<float, 3> box_min{};
  std::array<float, 3> box_max{};
  std::vector<std::string> textures;
  uint32_t gr2_bytes = 0;
  uint32_t geom_bytes = 0;
  float anim_period_sec = 8.0f;
  bool is_world_space = false;
  std::vector<AnimatedPgeoSubmesh> submeshes;
};

// Parses an FH1 Spectators.zip `.skinbin` skeletal mesh (big-endian).
bool parse_skinbin(std::span<const uint8_t> data, SpectatorMeshLod& out, int preferred_lod = 1);

// Parses an FH1 Spectators.zip `.anim.bin` clip header (duration, bone count, frame count).
bool parse_spectator_anim_clip(std::span<const uint8_t> data, const std::string& name, SpectatorAnimClip& out);

// Parses an FH1 animatedobjects.zip kind-4 `.pgeo` animated object header, bounds,
// texture references, and (when `decode_geometry` is true) half-float mesh geometry.
bool parse_animated_pgeo(std::span<const uint8_t> data, AnimatedPgeoObject& out, bool decode_geometry = true);

// Quantizes a viewer azimuth angle (radians, 0 = facing viewer) into [0, num_angles).
int select_impostor_angle(float rel_azimuth_rad, int num_angles, bool both_sides);

struct SpectatorModelDef {
  std::string name;
  std::string texture;
  int sprites_per_tex = 9;
};

struct SpectatorSkeletonDef {
  std::string name;
  std::string suffix;
  std::string subdir;
  std::vector<std::string> animations;
};

struct SpectatorCrowdClass {
  std::string name;
  std::string skeleton;
  std::string modelset;
  std::vector<std::string> idle_anims;
  std::vector<std::string> cheer_anims;
};

struct SpectatorCatalog {
  static constexpr uint32_t kAtlasGrid = 8;
  static constexpr uint32_t kAtlasCell = 256;
  static constexpr uint32_t kAtlasSize = kAtlasGrid * kAtlasCell;  // 2048
  static constexpr uint32_t kAtlasMipLevels = 9;                   // 2048..8 (stops at 1x1 per cell, zero bleed)
  static constexpr uint32_t kMaxDiffuseSlots = 20;                 // all 20 FH1 spectator textures (10M + 10F)
  static constexpr uint32_t kImpostorBaseSlot = 20;                // slots 20..29: 10-angle impostor slices
  static constexpr uint32_t kMaxImpostorSlots = 10;
  static constexpr uint32_t kNightLitSlot = 60;                    // warm overhead festival/outpost stage irradiance
  static constexpr uint32_t kNightDimSlot = 61;                    // subtle roadside dusk/moonlight irradiance
  static constexpr uint32_t kBlackSlot = 63;                       // pure black (unlit)
  static constexpr size_t kStandBoneCount = 19;
  static constexpr size_t kSitBoneCount = 15;

  int num_sprite_angles = 10;
  bool sprites_both_sides = false;
  std::vector<SpectatorModelDef> models;
  std::unordered_map<std::string, std::vector<std::string>> modelsets;
  std::vector<SpectatorSkeletonDef> skeletons;
  std::vector<SpectatorCrowdClass> crowdclasses;
  std::vector<SpectatorAsset> assets;
  std::vector<SpectatorAsset> sit_assets;
  std::vector<uint8_t> atlas_rgba;
  bool has_impostor_sprites = false;
  std::array<SpectatorStandBone, kStandBoneCount> stand_skeleton{};
  bool has_stand_skeleton_data = false;
  std::array<SpectatorStandBone, kSitBoneCount> sit_skeleton{};
  bool has_sit_skeleton_data = false;
  std::unordered_map<std::string, SpectatorAnimClip> anim_clips;
  std::vector<AnimatedPgeoObject> pgeo_objects;

  // Member counts from retail archives (verification & telemetry).
  size_t spectators_anim_bins = 0;
  size_t spectators_skinbins = 0;
  size_t spectators_xds = 0;
  size_t animated_pgeos = 0;
  size_t animated_xds = 0;

  bool has_3d_spectators() const {
    return !assets.empty() && atlas_rgba.size() == size_t(kAtlasSize) * kAtlasSize * 4;
  }
  const AnimatedPgeoObject* find_pgeo(const std::string& name_substr) const;
  bool load_spectators_xml(const std::string& xml);
  bool load_from_media(const std::filesystem::path& media_dir, std::string* error = nullptr);
};

struct HotAirBalloon {
  std::array<float, 3> base_pos{};  // game frame
  float radius = 7.5f;
  float height = 17.5f;
  float phase = 0.0f;
  std::array<uint8_t, 3> primary_rgb{232, 62, 46};
  std::array<uint8_t, 3> secondary_rgb{248, 214, 58};
  std::array<uint8_t, 3> accent_rgb{42, 132, 214};
};

struct WindmillProp {
  std::array<float, 3> base_pos{};  // game frame
  float yaw = 0.0f;                 // resting facing direction (aligns with wind when blowing)
  float tower_height = 14.0f;
  float blade_radius = 4.2f;
  float phase = 0.0f;
  bool is_turbine = false;          // false = ranch multi-vane windmill, true = 3-blade ridge wind turbine
};

struct FestivalFlagProp {
  std::array<float, 3> base_pos{};  // game frame
  float pole_height = 7.5f;
  float width = 1.6f;
  float height = 3.2f;
  float phase = 0.0f;
  std::array<uint8_t, 3> primary_rgb{236, 48, 92};
  std::array<uint8_t, 3> stripe_rgb{250, 210, 44};
};

struct FerrisWheelProp {
  std::array<float, 3> base_pos{};  // game frame
  float yaw = 0.0f;
  float radius = 18.0f;
  float hub_height = 23.5f;
  int gondolas = 12;
  float phase = 0.0f;
};

struct StageLightProp {
  std::array<float, 3> base_pos{};  // game frame
  float yaw = 0.0f;
  float beam_length = 42.0f;
  float phase = 0.0f;
  std::array<uint8_t, 3> beam_rgb{236, 52, 168};
};

struct AircraftBannerProp {
  std::array<float, 3> center_pos{};  // orbit center in game frame (y = cruise altitude)
  float orbit_radius = 400.0f;
  float angular_speed = 0.065f;       // rad/s
  float phase = 0.0f;
  bool is_blimp = false;              // true = Horizon Festival blimp, false = banner-towing plane
  std::array<uint8_t, 3> primary_rgb{236, 46, 92};
  std::array<uint8_t, 3> accent_rgb{252, 212, 48};
};

struct TrafficLightProp {
  std::array<float, 3> base_pos{};  // pole base on road shoulder (game frame)
  float yaw = 0.0f;                 // mast arm horizontal direction
  float pole_height = 6.8f;
  float arm_length = 4.4f;
  float phase_offset = 0.0f;        // seconds offset into 30 s Green/Yellow/Red cycle
};

// Smooth Green/Yellow/Red emissive weights [0..1] for an intersection traffic light cycle (30 s period).
struct TrafficLightWeights {
  float red = 0.0f;
  float yellow = 0.0f;
  float green = 1.0f;
};
TrafficLightWeights evaluate_traffic_light_cycle(float time_sec, float phase_offset = 0.0f);

struct CrowdCluster {
  std::array<float, 3> origin{};
  float dir_x = 1.0f;
  float dir_y = 0.0f;
  float dir_z = 0.0f;
  float face_x = 0.0f;
  float face_z = 1.0f;
  int count = 12;
  float spacing = 1.15f;
  std::string crowdclass = "GigCheer01";
  bool require_offroad = false;
  bool stage_lit = false;  // illuminated by festival/outpost stage floodlights at dusk & night
};

// Wheel angular velocity (rad/s) -> rim/spoke motion blur blend [0..1].
// Matches FH1's blur_rim / chrome_blur_rim transition speed band.
inline float wheel_blur_factor(float omega_rad_s) {
  const float w = omega_rad_s < 0.0f ? -omega_rad_s : omega_rad_s;
  const float t = (w - 15.0f) / 30.0f;
  return t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
}

struct AnimatedWorldPlacement {
  std::string pgeo_name;
  AnimatedPgeoCategory category = AnimatedPgeoCategory::kOther;
  std::array<float, 3> base_pos{};  // game frame
  float yaw = 0.0f;
  float scale = 1.0f;
  float phase = 0.0f;
  std::array<uint8_t, 3> primary_rgb{232, 62, 46};
  std::array<uint8_t, 3> secondary_rgb{248, 214, 58};
  std::array<uint8_t, 3> accent_rgb{42, 132, 214};
};

struct SpectatorFrustum {
  // 5 inward-facing unit planes in scenery space (left, right, bottom, top, in-front):
  // dot(n, p - origin) + d >= -radius to be visible.
  std::array<std::array<float, 4>, 5> planes{};
  double origin_x = 0.0;
  double origin_z = 0.0;
  bool active = false;
};

struct SpectatorFrameCache {
  bool valid = false;
  uint64_t world_gen = 0;
  double origin_x = 0.0;
  double origin_z = 0.0;
  double eye_x = 0.0;
  double eye_y = 0.0;
  double eye_z = 0.0;
  std::array<float, 3> cam_fwd{0.0f, 0.0f, 1.0f};
  float time_sec = -1.0f;
  float night_f = -1.0f;
  float nearest_dist = 1e6f;
  uint32_t spec_verts = 0;
  uint32_t shadow_verts = 0;
  std::vector<std::array<float, 4>> shadow_discs;
};

class AnimatedWorldSystem {
 public:
  AnimatedWorldSystem();
  ~AnimatedWorldSystem();

  bool init(const std::filesystem::path& media_dir, const TrackPostConfig& post_cfg, std::string* error = nullptr,
            const std::filesystem::path& world_dir = {});
  // Switches world context (Colorado vs ColoradoDirt Rally) and post config without
  // re-decoding Spectators.zip / animatedobjects.zip archives.
  void set_world(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
                 const TrackPostConfig& post_cfg);
  void set_post_config(const TrackPostConfig& post_cfg);
  const TrackPostConfig& post_config() const;
  const SpectatorCatalog& catalog() const;
  bool is_rally_world() const;

  // Shared primary instance linkage so DriveApp and Showroom share one initialized
  // AnimatedWorldSystem (catalog, atlas, pgeo objects, crowds, and real TrackPostConfig)
  // without duplicate archive loading.
  static void set_shared(AnimatedWorldSystem* instance);
  static AnimatedWorldSystem* shared();
  void share_from(const AnimatedWorldSystem& other);

  const std::vector<HotAirBalloon>& balloons() const;
  const std::vector<WindmillProp>& windmills() const;
  const std::vector<FestivalFlagProp>& flags() const;
  const std::vector<FerrisWheelProp>& ferris_wheels() const;
  const std::vector<StageLightProp>& stage_lights() const;
  const std::vector<AircraftBannerProp>& aircraft_banners() const;
  const std::vector<TrafficLightProp>& traffic_lights() const;
  const std::vector<CrowdCluster>& crowds() const;
  const std::vector<AnimatedWorldPlacement>& pgeo_placements() const;

  // Active terrain height sampler observed during build_mesh() so spectator
  // grounding in Showroom::record_debris_draw() always has real terrain elevation.
  static const drive::Ground* last_ground();

  // Appends animated world triangles (.pgeo animated props, hot-air balloons,
  // Rally stage props, and wind-swaying roadside grass quads) in game-frame coordinates.
  void build_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                  std::vector<DebrisVertex>& out, bool include_grass = true) const;

  // Appends authentic 3D textured Spectators.zip crowd triangles (standing, sitting,
  // .anim.bin clip-modulated, and distant 10-angle impostors) in game-frame coordinates.
  void build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                            std::vector<SpectatorVertex>& out) const;
  void build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                            std::vector<SpectatorVertex>& out,
                            std::vector<std::array<float, 4>>* shadow_discs) const;
  void build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                            std::vector<SpectatorVertex>& out,
                            std::vector<std::array<float, 4>>* shadow_discs,
                            const SpectatorFrustum* frustum,
                            float* out_nearest_dist = nullptr) const;

  void invalidate_spectator_ground_cache() const;
  size_t cached_spectator_ground_count() const;
  void invalidate_grass_ground_cache() const;
  size_t cached_grass_ground_count() const;
  SpectatorFrameCache& frame_cache() { return frame_cache_; }
  const SpectatorFrameCache& frame_cache() const { return frame_cache_; }
  void reset_frame_cache() { frame_cache_ = SpectatorFrameCache{}; }

 private:
  struct CachedSpectatorGround {
    float y = 0.0f;
    bool valid = false;
    bool culled = false;
  };
  struct CachedGrassCell {
    float y = 0.0f;
    bool grow = false;
  };
  struct SharedState {
    SpectatorCatalog catalog;
    TrackPostConfig post_cfg;
    bool rally_world = false;
    std::vector<HotAirBalloon> balloons;
    std::vector<WindmillProp> windmills;
    std::vector<FestivalFlagProp> flags;
    std::vector<FerrisWheelProp> ferris_wheels;
    std::vector<StageLightProp> stage_lights;
    std::vector<AircraftBannerProp> aircraft_banners;
    std::vector<TrafficLightProp> traffic_lights;
    std::vector<CrowdCluster> crowds;
    std::vector<AnimatedWorldPlacement> pgeo_placements;

    // Cached per-spectator & per-cell grass/prop ground heights & prebuilt catalog lookup pools
    // (eliminates ~800/frame CPU BVH raycasts while auto-invalidating on world swap / Ground* change).
    mutable std::mutex cache_mutex;
    mutable const drive::Ground* cached_ground_ptr = nullptr;
    mutable std::vector<std::vector<CachedSpectatorGround>> crowd_ground_cache;
    mutable const drive::Ground* cached_grass_ground_ptr = nullptr;
    mutable std::unordered_map<uint64_t, CachedGrassCell> grass_ground_cache;
    mutable std::unordered_map<uint64_t, float> prop_ground_cache;
    mutable bool pools_ready = false;
    mutable size_t pools_stand_count = 0;
    mutable size_t pools_sit_count = 0;
    mutable size_t pools_cc_count = 0;
    mutable std::vector<size_t> all_stand_pool;
    mutable std::vector<size_t> male_stand_pool;
    mutable std::vector<size_t> all_sit_pool;
    mutable std::vector<size_t> male_sit_pool;
    mutable std::unordered_map<std::string, const SpectatorCrowdClass*> cc_by_low;
  };

  void populate_default_colorado();
  void populate_pgeo_placements();
  void populate_gameobjs_crowds(const std::filesystem::path& media_dir);
  void populate_rally_world(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir);

  std::shared_ptr<SharedState> state_;
  SpectatorFrameCache frame_cache_{};
};

}  // namespace world
