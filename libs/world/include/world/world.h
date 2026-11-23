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
// Whether an effect represents a water surface (LAKE_*, *WATER*, *RIVER*, *POND*).
bool is_water_effect(std::string_view effect);
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
  // Additional material slots (WORLD-MATERIALS, Slot roles):
  //   light_map: slot 7 on _LM effects (additive night spill, never multiplied!)
  //   ao_map: AO / AOMap (slots 4, 5, or 6)
  //   normal_map: NormalMap (slot 1 on H_DIFF_*NORM*, slot 3 on roads/terrain, slot 1 on LAKE)
  //   layer_c: Blend_C (slot 5 on H_BLND3 / H_ROAD_DIFF3)
  //   spec_map: SpecularMap (when present)
  uint32_t light_map = kNoTexture;
  uint32_t ao_map = kNoTexture;
  uint32_t normal_map = kNoTexture;
  uint32_t layer_c = kNoTexture;
  uint32_t spec_map = kNoTexture;
  std::array<float, 2> tiling_c{1, 1};  // pixel constant 1 xy (Blend_C tiling)
  float emissive_force = 0.0f;          // pixel constant 3 w on _LM effects
  uint8_t is_water = 0;                 // LAKE_* water effect
  uint8_t anim_flag = 0;                // ANIM_FLAG* waving cloth flag effect
  uint8_t is_grass = 0;                 // _0x00005007 grass quad / grass shell effect
  uint8_t is_road = 0;                  // H_ROAD* road surface (RoadSpecular / RoadGlossiness)
  std::array<float, 2> decal_noise_c4{3.5f, 8.0f};  // H_VBLND_DECAL* pixel constant 4 xy
  // The far ring (ours): 0 near scenery, 1 a MIDDIST object, 2 an UberLOD
  // one. The renderer pushes far levels back in depth so the near terrain
  // wins where both are drawn.
  uint8_t far = 0;
  // The distance cross-fade (ours; GatherOptions::prop_near_radius): 0
  // always drawn, 1 a prop's LOD00 (faded out past the LOD distance), 2 its
  // LOD02 (faded in there, out at the gather's edge), 3 a prop with one
  // level (faded out at the edge).
  uint8_t fade = 0;
  // The census (not part of the key): the raw slot values of the first
  // submesh that made the material (-2: no slot) and their records' types
  // (-1: no record).
  std::array<int32_t, 3> slot_values{-2, -2, -2};
  std::array<int32_t, 3> slot_types{-1, -1, -1};
  auto key() const {
    return std::make_tuple(effect, albedo, far, layer_b, splat, tiling, fade, light_map, ao_map, normal_map, layer_c,
                           spec_map, tiling_c, is_water, anim_flag, is_grass, is_road, decal_noise_c4);
  }
};

// Staggered night streetlamp / festival / outpost glow sprite (SPEC-SKY-AND-TOD §6.3).
struct WorldGlow {
  std::array<float, 3> position{};  // game frame
  std::array<float, 3> color{1.0f, 0.78f, 0.42f};
  float radius = 1.6f;
  float threshold = 0.35f;          // switches on when SwitchOnLights >= threshold
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
  // Splat tiling (pixel constant 1's zw). uv2: always the lightmap / AO UV
  // (TEXCOORD2 on H_BLND2/3 so AO & Light_Map sample separately from Splat).
  // colours: as stored, except on H_VBLND effects, whose R is the layer
  // weight (or the decal fade) saturate((COLOR - 0.5) x c4.x + 0.5), COLOR.x
  // (the second stored byte, R here) for the layer weight, COLOR.w (the
  // first, A here) for the fade (kept intact in A for H_VBLND_DECAL noise).
  std::vector<std::array<float, 2>> uv0, uv1, uv2;
  std::vector<std::array<uint8_t, 4>> colours;
  std::vector<uint32_t> material;  // per triangle: index into materials
  // Per triangle: its prop's placement origin (game frame), so the
  // renderer can pick a level per instance; zero for scenery.
  std::vector<std::array<float, 3>> instance;
  std::vector<MeshMaterial> materials;
  std::vector<WorldGlow> world_glows;
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
  // Named props (lower-case GameplayIDs) left out: found discount signs,
  // a barn's closed or open doors (SPEC-FREEROAM-COLLECTIBLES). Null: none.
  std::shared_ptr<const std::set<std::string>> hidden;
  double radius = 600;       // world-space models whose header box comes within this (x-z)
  double prop_radius = 250;  // props placed within this
  double prop_lod0 = 80;     // props nearer than this use LOD00, the rest LOD02 (or their last)
  // Above 0 (ours; needs render_data): every prop takes LOD02 (or its last)
  // and, within this, LOD00 as well, their materials marked for the
  // renderer's distance cross-fade (MeshMaterial::fade) in place of the
  // switch at prop_lod0.
  double prop_near_radius = 0;
  bool props = true;
  bool render_data = false;  // fill the Mesh's normals, UVs, colours and materials
  // The far ring (ours; needs render_data): the MIDDIST and UberLOD terrain
  // the near gather skips, from models whose header box comes within this,
  // finest LOD per family; their triangles wholly within far_inner of the
  // point (MIDDIST) or far_inner_uberlod (UberLOD) are left out. 0: none.
  // MIDDIST's 150 m (ours): some MIDDIST hills have no near terrain over
  // them (MIDDIST_06 at 353 m in the hill frame, WORLD_MATERIALS Q44e),
  // and the gather point can sit a regather step from the eye.
  double far_radius = 0;
  double far_inner = 150;
  double far_inner_uberlod = 800;
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
  size_t props_hidden = 0;   // named props left out (GatherOptions::hidden)
  size_t props_markers = 0;  // placements of the Temp_Arrow_Pointer marker, left out
  size_t props_beacons = 0;  // placements of the gameplay beacon (coloradoout.06509), left out
  size_t props_near_lod = 0;  // with prop_near_radius: the LOD00 copies added beside LOD02
  std::map<std::string, size_t> materials;  // triangles per material name
  std::map<std::string, size_t> object_names;  // world-space triangles per mesh object name
  // With render_data: models drawn without a uvOffsetScale (given offset 0
  // and scale 1, the engine's choice) and submeshes without a material.
  std::set<std::string> default_uv_models;
  size_t no_material = 0;
};

// A world folder's own file: Ribbon_00/<folder name><suffix> found
// without case (Colorado_00.pvs in colorado/, ColoradoDirt_track_00.col in
// ColoradoDirt/); the literal name when there is no such file.
std::filesystem::path ribbon_file(const std::filesystem::path& world, const std::string& suffix);
// Its render models' prefix: the folder name in lower case and "out".
std::string model_prefix(const std::filesystem::path& world);

class World {
 public:
  World();
  ~World();
  // `colorado`: the folder holding bin.zip and Ribbon_00/Colorado_00.pvs.
  // `cache`: a file for the model index (header boxes); built on first use
  // (a full parse of every model) and reused while bin.zip's size matches.
  bool open(const std::filesystem::path& colorado, const std::filesystem::path& cache, std::string* error);
  // The world's name (its folder: colorado, ColoradoDirt) and its files.
  const std::string& name() const { return name_; }
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
  std::string name_;
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
