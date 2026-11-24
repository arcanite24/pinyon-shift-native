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
};

struct SpectatorAsset {
  std::string name;
  SpectatorMeshLod mesh;
  uint32_t atlas_slot = 0;  // cell index [0..19] in 8x8 RGBA8 atlas
  bool is_male = true;
};

struct SpectatorStandBone {
  int16_t parent = -1;
  std::array<float, 3> bind_world{};
};

// Parses an FH1 Spectators.zip `.skinbin` skeletal mesh (big-endian).
bool parse_skinbin(std::span<const uint8_t> data, SpectatorMeshLod& out, int preferred_lod = 1);

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
};

struct SpectatorCatalog {
  static constexpr uint32_t kAtlasGrid = 8;
  static constexpr uint32_t kAtlasCell = 256;
  static constexpr uint32_t kAtlasSize = kAtlasGrid * kAtlasCell;  // 2048
  static constexpr uint32_t kAtlasMipLevels = 9;                   // 2048..8 (stops at 1x1 per cell, zero bleed)
  static constexpr uint32_t kMaxDiffuseSlots = 20;                 // all 20 FH1 spectator textures (10M + 10F)
  static constexpr uint32_t kNightLitSlot = 60;                    // warm overhead festival/outpost stage irradiance
  static constexpr uint32_t kNightDimSlot = 61;                    // subtle roadside dusk/moonlight irradiance
  static constexpr uint32_t kBlackSlot = 63;                       // pure black (unlit)
  static constexpr size_t kStandBoneCount = 19;

  int num_sprite_angles = 10;
  bool sprites_both_sides = false;
  std::vector<SpectatorModelDef> models;
  std::unordered_map<std::string, std::vector<std::string>> modelsets;
  std::vector<SpectatorSkeletonDef> skeletons;
  std::vector<SpectatorCrowdClass> crowdclasses;
  std::vector<SpectatorAsset> assets;
  std::vector<uint8_t> atlas_rgba;
  std::array<SpectatorStandBone, kStandBoneCount> stand_skeleton{};
  bool has_stand_skeleton_data = false;

  // Member counts from retail archives (verification & telemetry).
  size_t spectators_anim_bins = 0;
  size_t spectators_skinbins = 0;
  size_t spectators_xds = 0;
  size_t animated_pgeos = 0;
  size_t animated_xds = 0;

  bool has_3d_spectators() const {
    return !assets.empty() && atlas_rgba.size() == size_t(kAtlasSize) * kAtlasSize * 4;
  }
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
  float yaw = 0.0f;                 // facing direction
  float tower_height = 14.0f;
  float blade_radius = 4.2f;
  float phase = 0.0f;
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

class AnimatedWorldSystem {
 public:
  AnimatedWorldSystem();
  ~AnimatedWorldSystem();

  bool init(const std::filesystem::path& media_dir, const TrackPostConfig& post_cfg, std::string* error = nullptr);
  void set_post_config(const TrackPostConfig& post_cfg) { post_cfg_ = post_cfg; }
  const TrackPostConfig& post_config() const { return post_cfg_; }
  const SpectatorCatalog& catalog() const { return catalog_; }

  const std::vector<HotAirBalloon>& balloons() const { return balloons_; }
  const std::vector<WindmillProp>& windmills() const { return windmills_; }
  const std::vector<FestivalFlagProp>& flags() const { return flags_; }
  const std::vector<CrowdCluster>& crowds() const { return crowds_; }

  // Active terrain height sampler observed during build_mesh() so spectator
  // grounding in Showroom::record_debris_draw() always has real terrain elevation.
  static const drive::Ground* last_ground();

  // Appends animated world triangles (hot-air balloons and wind-swaying
  // roadside grass quads) in game-frame coordinates to `out`.
  void build_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                  std::vector<DebrisVertex>& out, bool include_grass = true) const;

  // Appends authentic 3D textured Spectators.zip festival crowd triangles
  // positioned behind festival barriers in game-frame coordinates.
  void build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                            std::vector<SpectatorVertex>& out) const;
  void build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec, const drive::Ground* ground,
                            std::vector<SpectatorVertex>& out,
                            std::vector<std::array<float, 4>>* shadow_discs) const;

 private:
  void populate_default_colorado();
  void populate_gameobjs_crowds(const std::filesystem::path& media_dir);

  SpectatorCatalog catalog_;
  TrackPostConfig post_cfg_;
  std::vector<HotAirBalloon> balloons_;
  std::vector<WindmillProp> windmills_;
  std::vector<FestivalFlagProp> flags_;
  std::vector<CrowdCluster> crowds_;
};

}  // namespace world
