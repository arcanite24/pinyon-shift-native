// Colorado around a point: the render models and props near it as one
// triangle soup, for drawing and (M3 stand-in) for driving on.
//
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md):
// world-space models are selected by their header box; local-space props
// are placed from the kind-7 .pgeo tiles and the .pvsz zones through the
// object table. Everything here is in the file frame (the game's world
// with z negated), which is also the drive's world frame: +Y up, and the
// same handedness as the car's body frame.
//
// Ours, not the spec's: the surface classes (by mesh object name, then
// material name), the LOD choice (the lowest LOD of each object family among
// the gathered models; props by distance), leaving out the geometry the
// main view does not draw near the car (TERR_CUBE models of the reflection
// views, the distant UberLOD and middle-distance terrain, occluders), and
// the streaming radii. The surface classes stand in for COLLISION.md's
// surfaces.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace world {

enum class Surface : uint8_t { kRoad, kTerrain, kWater, kOther, kProp };
const char* surface_name(Surface s);
// Our guess from a mesh object's name and a submesh's material name (any
// case): the object name first (Colorado's roads are "..._Road_LODnn_..."
// objects), then the material.
Surface classify(const std::string& object, const std::string& material);
// Whether a world-space object or submesh is left out of the gather.
bool skipped_object(const std::string& object);
bool skipped_material(const std::string& material);
// Whether a world-space object is driven on in the stand-in ground. Thin,
// upright things (armco, barriers, railings, cables, gates, fences) are
// not: with nothing but wheel rays they act as ramps. Props never are.
bool ground_object(const std::string& object);

struct Mesh {
  std::vector<std::array<float, 3>> positions;  // three per triangle, file frame
  std::vector<Surface> surface;                 // per triangle
  std::vector<uint32_t> model;                  // per triangle: index into World::models()
  std::vector<uint8_t> ground;                  // per triangle: part of the stand-in ground
  size_t triangles() const { return surface.size(); }
};

struct ModelInfo {
  std::string name;  // coloradoout.NNNNN.rmb.bin
  std::array<float, 3> box_min{}, box_max{};
  bool world_space = false;
  bool ok = false;  // parsed
};

struct GatherOptions {
  double radius = 600;       // world-space models whose header box comes within this (x-z)
  double prop_radius = 250;  // props placed within this
  double prop_lod0 = 80;     // props nearer than this use LOD00, the rest LOD02 (or their last)
  bool props = true;
};

struct GatherStats {
  size_t world_models = 0, objects = 0, skipped_lod = 0, skipped_far = 0, props = 0, prop_unresolved = 0;
  std::map<std::string, size_t> materials;  // triangles per material name
  std::map<std::string, size_t> object_names;  // world-space triangles per mesh object name
};

class World {
 public:
  World();
  ~World();
  // `colorado`: the folder holding bin.zip and Ribbon_00/Colorado_00.pvs.
  // `cache`: a file for the model index (header boxes); built on first use
  // (a full parse of every model) and reused while bin.zip's size matches.
  bool open(const std::filesystem::path& colorado, const std::filesystem::path& cache, std::string* error);
  const std::vector<ModelInfo>& models() const { return models_; }
  size_t placement_count() const;

  // Every world-space model near (x, z) and every prop placement near it.
  bool gather(double x, double z, const GatherOptions& options, Mesh& out, GatherStats* stats,
              std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<ModelInfo> models_;
};

// A stand-in spawn: the centre of the upward-facing road triangle nearest
// (x, z), heading along the triangle's longest edge (of its two directions,
// the one nearer `prefer_heading` when given, else the one nearer +z), on
// the drive's heading convention (0 faces +z, positive turns towards +x).
// Returns false when the mesh has no road.
struct Spawn {
  std::array<double, 3> position{};
  double heading = 0;
  double distance = 0;  // from (x, z), metres
};
bool find_spawn(const Mesh& mesh, double x, double z, Spawn& out, const double* prefer_heading = nullptr);

}  // namespace world
