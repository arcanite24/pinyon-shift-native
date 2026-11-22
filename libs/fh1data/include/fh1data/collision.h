// Colorado's collision: the `N.fiz` tiles in bin.zip, the `.col` index that
// places them on a lattice, and the surface table the triangles index.
// Spec: COLLISION.md (horizon1-recomp docs/formats, 43346e1d). Everything is
// big-endian and already in the game frame (no z negation).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fh1 {

// One `N.fiz` tile: header, block A (vertices, triangles, a 12 x 12 grid of
// 10 m cells) and block B (`OWBB`, skipped by its size).
struct FizTriangle {
  uint8_t flags = 0;    // not decoded
  uint8_t surface = 0;  // index into the surface list
  // The route word (COLLISION.md, Route groups): 0x8000 collides whatever
  // route is active; bit i (i < 15) while route i of the tile's list is.
  uint16_t word = 0;
  uint32_t edges = 0;   // offset 28, not decoded (0x00FFFFFF on most)
  std::array<uint32_t, 3> v{};  // vertex indices
  std::array<float, 3> normal{};  // unit, (b - a) x (c - a) normalised
};

struct FizTile {
  uint32_t version = 0;
  uint32_t block_a_size = 0, block_b_size = 0, block_b_records = 0;
  uint32_t grid_x = 0, grid_z = 0;
  std::array<float, 4> box{};   // xmin, zmin, xmax, zmax
  std::array<float, 2> cell{};  // cell size x, z
  // The tile's route list (block A +60, as many as the count at +122):
  // track route numbers, bit i of a triangle's word naming entry i.
  std::vector<uint16_t> routes;
  std::vector<std::array<float, 3>> vertices;
  std::vector<FizTriangle> triangles;
  // Per grid cell (x fastest), the triangle indices listed in it.
  std::vector<std::vector<uint16_t>> grid;
};

// Parses a tile; false and `error` on a malformed one.
bool parse_fiz(std::span<const uint8_t> data, FizTile& tile, std::string* error);

// `Ribbon_00/Colorado_track_00.col`: the lattice and one record per tile.
struct ColRecord {
  uint32_t stream_offset = 0;
  uint32_t tile = 0;  // N of N.fiz
  uint32_t cell = 0;  // j * width + i
  uint32_t block_size = 0;
};

struct ColIndex {
  uint32_t width = 0, height = 0;  // cells in x (i) and z (j)
  float x0 = 0, z0 = 0;            // lattice origin, game frame
  float cell_size = 100;           // m (COLLISION.md lattice)
  std::vector<ColRecord> records;
};

bool parse_col(std::span<const uint8_t> data, ColIndex& index, std::string* error);

// `surfaceTypes.xml` (physics.zip): the list order gives the indices; each
// surface's definition its category and FrictionScale. An index with no
// definition gets Asphalt's values (COLLISION.md name quirk) and `defined`
// false.
struct SurfaceType {
  std::string name;
  std::string category;
  float friction_scale = 1;
  // The tyre's columns (SPEC-CAR-PHYSICS, Tyre model: Off-road surfaces).
  float rear_grip = 1;         // RearGripMultiplier, on the rear wheels
  float off_roadness = 0;      // OffRoadness
  float off_road_peak_sa = 12; // OffRoadDryPeakSA, degrees
  float handbrake_grip = 1;    // HandbrakeGripMultiplier
  float tcs_speed_scale = 1;   // TCandStabilitySpeedMultiplier
  // Friction/VelDepFriction (SPEC-CAR-PHYSICS, Surface drag).
  float vel_peak0 = 0, vel_peak = 8;
  float vel_long0 = 0, vel_long = 0.01f;
  float vel_lat0 = 0, vel_lat = 0.03f;
  // Bumpiness (COLLISION.md, Runtime SurfaceType evaluation).
  int bump_noise_type = 0;
  float bump_frequency = 0;
  float bump_clamp_min = -1, bump_clamp_max = 1;
  float bump_scale = 0, bump_offset = 0;
  float bump_rumble = 0;
  // Springiness (COLLISION.md, soft-ground contact).
  bool is_springy = false;
  float spring_k = 0, spring_comp_d = 0, spring_reb_d = 0;
  float spring_norm_y_cutoff = 0.707f, spring_speed_cutoff = 4.0f;
  // Flags (COLLISION.md).
  bool is_penalty_surface = false;
  bool is_hand_of_god = false;
  // Skidmarks, tyre smoke/dust/gravel plumes & sparks (SPEC-VFX-AND-PARTICLES).
  uint8_t skid_r = 0, skid_g = 0, skid_b = 0, skid_a = 0;
  int skid_texture_index = 0;  // 0 = asphalt rubber, 1 = off-road tyre rut
  float skid_min_intensity = 0.0f;
  float smoke_weight = 0.0f;
  float dust_weight = 0.0f;
  float dirt_weight = 0.0f;
  float grass_weight = 0.0f;
  float gravel_weight = 0.0f;
  float leaf_weight = 0.0f;
  float litter_weight = 0.0f;
  bool should_spark = false;
  bool defined = false;
};

bool parse_surface_types(std::string_view xml, std::vector<SurfaceType>& out, std::string* error);

}  // namespace fh1
