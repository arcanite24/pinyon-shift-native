// Colorado around a point: the render models and props near it as one
// triangle soup, for drawing and (M3 stand-in) for driving on.
//
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md):
// world-space models are selected by their header box; local-space props
// are placed from the kind-7 .pgeo tiles and the .pvsz zones through the
// object table. The files hold the file frame; the game's world is the
// file frame with z negated. Everything here (points in, triangles out) is
// in the game frame, which is the drive's world frame: the car's body frame
// has its handedness (AUTOSHOW_REFERENCE's car_to_world negates the model
// frame's z, as FORMAT-CARS's model-to-body pose does), so the body is
// placed in it by a rotation. The z negation happens once, here.
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
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include "fh1data/placements.h"
#include "fh1data/world_texture.h"

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

// How a material draws (WORLD-MATERIALS, the frame's blend state per
// effect family): opaque; opaque with alpha to coverage (props' H_DIFF and
// trees, the cut-out in the diffuse alpha); blended with the source alpha
// (road decals, vertex-blend decals, the lake). Other effects named _OPAC
// also get alpha to coverage (ours: H_VBLND_NORM_MASK_OPAC_BIAS_LM's texture
// has cut-out alpha; the frame showed none of its draws).
// kAdditive (interim, ours): DIFF_GLOW_* glow cards, added unlit over what
// is behind with no depth write, until WORLD-MATERIALS gives their blend
// state (their textures are black where nothing glows).
enum class Blend : uint8_t { kOpaque, kCoverage, kAlpha, kAdditive };
Blend effect_blend(const std::string& effect);
// Whether an effect lies on another surface and is drawn with a depth bias
// (ours): H_VBLND_DECAL*, any *_DECAL*, and the blended road and decal
// families (kAlpha but the lake).
bool decal_effect(const std::string& effect);

// A gathered material: its effect and the texture records (indices into
// World::textures(), kNoTexture when the slot names no file) of the slots
// drawn so far.
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
// How a material's layers combine (WORLD-MATERIALS, Terrain shading).
enum class Layers : uint8_t {
  kOne,          // slot 0 alone
  kVertexBlend,  // H_VBLND: lerp(Blend_A, Blend_B, the vertex colour's R)
  kSplat,        // H_BLND2, H_BLND3: lerp(Blend_A, Blend_B, Splat's R at uv1)
  kVertexFade,   // H_VBLND_DECAL: slot 0, its alpha times the vertex colour's R
  // A tree sprite (WORLD-MATERIALS, Tree billboards): every corner holds the
  // base point; the normal is the trunk axis, uv1 the corner's offset
  // (across in metres, up in metres), the colour the tint (times 2), and
  // the renderer turns the quad to the camera.
  kBillboard,
};

struct MeshMaterial {
  std::string effect;               // upper case, e.g. H_ROAD_DIFF3_NORM1_NOISE_AO_LM
  uint32_t albedo = kNoTexture;     // slot 0: Diffuse_Texture or Blend_A
  Blend blend = Blend::kOpaque;
  Layers layers = Layers::kOne;
  uint32_t layer_b = kNoTexture;    // slot 1: Blend_B (kVertexBlend, kSplat)
  uint32_t splat = kNoTexture;      // slot 2: Splat (kSplat)
  // Pixel constant 0: Blend_A's UV tiling in xy, Blend_B's in zw (terrain
  // effects; 1 elsewhere).
  std::array<float, 4> tiling{1, 1, 1, 1};
  // The far ring (ours): 0 near scenery, 1 a MIDDIST object, 2 an UberLOD
  // one. The renderer pushes far levels back in depth so the near terrain
  // wins where both are drawn.
  uint8_t far = 0;
  auto key() const { return std::make_tuple(effect, albedo, far, layer_b, splat, tiling); }
};

struct Mesh {
  std::vector<std::array<float, 3>> positions;  // three per triangle, game frame
  std::vector<Surface> surface;                 // per triangle
  std::vector<uint32_t> model;                  // per triangle: index into World::models()
  std::vector<uint8_t> ground;                  // per triangle: part of the stand-in ground
  size_t triangles() const { return surface.size(); }
  // With GatherOptions::render_data: per corner (parallel to positions) the
  // normal (game frame, zero when the layout has none), the surface UV and
  // the lightmap UV (dequantised with the submesh's uvOffsetScale), the
  // vertex colour (R, G, B, A; white when none); per triangle its material.
  std::vector<std::array<float, 3>> normals;
  // uv1: the lightmap UV, or for kSplat materials TEXCOORD1 times the
  // Splat tiling (pixel constant 1's zw). colours: as stored, except on
  // H_VBLND effects, whose R is the layer weight (or the decal fade)
  // saturate((COLOR - 0.5) x c4.x + 0.5), COLOR.x (the second stored byte,
  // R here) for the layer weight, COLOR.w (the first, A here) for the fade.
  std::vector<std::array<float, 2>> uv0, uv1;
  std::vector<std::array<uint8_t, 4>> colours;
  std::vector<uint32_t> material;  // per triangle: index into materials
  std::vector<MeshMaterial> materials;
  bool has_render_data() const { return !material.empty(); }
};

struct ModelInfo {
  std::string name;  // coloradoout.NNNNN.rmb.bin
  std::array<float, 3> box_min{}, box_max{};
  bool world_space = false;
  bool ok = false;   // parsed
  bool far = false;  // holds MIDDIST or UberLOD objects (the far ring)
};

struct GatherOptions {
  double radius = 600;       // world-space models whose header box comes within this (x-z)
  double prop_radius = 250;  // props placed within this
  double prop_lod0 = 80;     // props nearer than this use LOD00, the rest LOD02 (or their last)
  bool props = true;
  bool render_data = false;  // fill the Mesh's normals, UVs, colours and materials
  // The far ring (ours; needs render_data): the MIDDIST and UberLOD terrain
  // the near gather skips, from models whose header box comes within this,
  // finest LOD per family; their triangles wholly within far_inner of the
  // point (MIDDIST) or 1.5 x far_inner (UberLOD) are left out. 0: none.
  double far_radius = 0;
  double far_inner = 400;
  // Tree sprites (needs render_data): the kind-7 tiles' billboard records
  // whose base is within this of the point and within their far distance
  // of it. 0: none.
  double tree_radius = 0;
  // The track route whose placements are drawn (WORLD-GEOMETRY, "Which
  // placements free roam draws"): 0 is free roam; -1 draws every one.
  int route = 0;
};

struct GatherStats {
  size_t world_models = 0, objects = 0, skipped_lod = 0, skipped_far = 0, props = 0, prop_unresolved = 0;
  size_t far_models = 0, far_triangles = 0;  // the far ring
  size_t trees = 0, trees_untextured = 0;    // tree sprites; those whose atlas has no file
  size_t props_off_route = 0, trees_off_route = 0;  // left out by the route rule
  size_t props_markers = 0;  // placements of the Temp_Arrow_Pointer marker, left out
  size_t props_beacons = 0;  // placements of the gameplay beacon (coloradoout.06509), left out
  std::map<std::string, size_t> materials;  // triangles per material name
  std::map<std::string, size_t> object_names;  // world-space triangles per mesh object name
  // With render_data: models drawn without a uvOffsetScale (given offset 0
  // and scale 1, the engine's choice) and submeshes without a material.
  std::set<std::string> default_uv_models;
  size_t no_material = 0;
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
  // The ribbon's texture records (WORLD-MATERIALS), by record index.
  const std::vector<fh1::PvsTexture>& textures() const;
  // Reads and untiles a texture record's files (type 4 .bix pair or type 0
  // CAFF .bin). False for records without a file and for files whose sizes
  // do not fit the layout (the spec's three undecoded CAFF textures). Uses
  // the archive: not while a gather runs on another thread.
  bool read_texture(uint32_t record, fh1::WorldTexture& out, std::string* error);

  // Every world-space model near (x, z) (game frame) and every prop
  // placement near it.
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

// The default search point and heading: the free-roam camera of
// WORLD_GEOMETRY's frame dump, by the festival (file frame 1999.0, 145.4,
// -962.9), in the game frame, looking the way that camera looked.
constexpr double kDumpCameraX = 1999.0, kDumpCameraZ = 962.9, kDumpCameraHeading = 2.199;

}  // namespace world
