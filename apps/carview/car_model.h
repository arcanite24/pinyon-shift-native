// Turns a car archive's model and body atlas into GPU-ready arrays.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fh1data/xds.h"

namespace carview {

enum class MaterialKind : uint32_t { kTextured = 0, kPaint = 1, kGlass = 2 };

struct Vertex {
  float position[3];
  float normal[3];
  float uv[2];
  float occlusion;
  float kind;  // MaterialKind
};
static_assert(sizeof(Vertex) == 40);

struct CarModel {
  std::string model_member;    // e.g. FOR_GT40_05_lod0.carbin
  std::string texture_member;  // e.g. nodamage_LOD0.xds
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;  // triangle list
  std::array<float, 3> bounds_min{}, bounds_max{};
  std::vector<fh1::RgbaImage> texture_levels;  // base first
  size_t parts = 0, hidden_parts = 0, subsections = 0, skipped_subsections = 0;
};

struct LoadOptions {
  bool cockpit = true;     // LOD 0: also draw `<car>_cockpit.carbin`
  bool all_parts = false;  // include upgrade variants and the unplaced wheel
};

// Loads `<game>/media/cars/<car>.zip`. lod 0 uses `<car>_lod0.carbin` (and
// `_cockpit`) with `nodamage_LOD0.xds`; lod 1-5 use `<car>.carbin` with
// `nodamage.xds`.
bool load_car(const std::filesystem::path& game_root, const std::string& car, int lod,
              const LoadOptions& options, CarModel& out, std::string& error);

}  // namespace carview
