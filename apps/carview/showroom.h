// carview showroom: draws one baked car (`carbake` output) with our own car
// shader: the default paint, livery masks, lamps, interior, wheels placed at
// the wheel-centre rows with rims, tyres, rotors and callipers, glass last,
// and our own lighting approximation (a procedural studio environment, a key
// and a fill light).
//
// Spec: FORMAT-CARS (Materials, Paint, Wheels, Brakes, Lights, glass),
// AUTOSHOW_REFERENCE (what the showroom draws).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <volk.h>

#include "cars/bake_format.h"
#include "cars/shader_settings.h"
#include "cars/visual_setup.h"
#include "drive/damage.h"
#include "drive/lamps.h"
#include "renderer.h"
#include "world/smashables.h"
#include "world/tod.h"
#include "world/vfx.h"
#include "world/world.h"
#include "world_gpu.h"
#include "world_texture_lru.h"

namespace carview {

// One wheel's pose for set_wheel_poses, in the body frame (FORMAT-CARS:
// +X right, +Y up, +Z forward): the hub centre, the steer angle about y
// (positive towards +x) and the rolling angle about x (positive rolling
// forward).
struct WheelPose {
  float centre[3] = {0, 0, 0};
  float steer = 0, spin = 0;
  float omega = 0;  // angular speed rad/s (drives blur_rim / chrome_blur_rim blend)
};

// The frame's uniform block (showroom.vert / showroom.frag, set 0 binding 0).
struct ShowroomFrame {
  float view_projection[16];  // car space to Vulkan clip, column-major
  float eye[4];               // camera position in car space
  float key_light[4];         // towards the light, intensity
  float fill_light[4];
  float options[4];  // exposure, environment intensity, livery overlay, metallic paint's metal share
  float tint[4];     // light colour (linear rgb) for diffuse light: environment and both lights
  float reflect_tint[4];  // colour of environment reflections (linear rgb)
  float transparent_view_projection[16];  // glass and lamp covers; usually view_projection
  float stripe[4];  // the factory stripe colour (linear rgb); set by the showroom
  // The drive's sun shadow (world.frag; set by the showroom each frame):
  // the frame's space to the map's clip, then on, texel, strength, taps,
  // and the normal offset in metres.
  float shadow_matrix[16];
  float shadow[4];
  float shadow_bias[4];
  // The world's distance cross-fade (world.frag; ours): the prop LOD
  // distance and its band, and where the props fade out at the gather's
  // edge (start, end), in metres from the eye. Band 0: off.
  float world_fade[4];
  // The drive's light (showroom.frag; set by set_world_light's state each
  // frame): xyz the world's up in the car frame, w 1 to light the car by
  // the world's sun and sky rather than the studio.
  float drive[4];
  // Dynamic 24-hour Time-of-Day lighting and fog (SPEC-SKY-AND-TOD).
  // sun_color[3] < 0 falls back to legacy fixed midday constants.
  float sun_color[4] = {0.9695f, 0.8358f, 0.5840f, -1.0f};
  float sky_color[4] = {0.1687f, 0.2229f, 0.2746f, 0.0f};
  float ground_color[4] = {0.1788f, 0.1729f, 0.1024f, 0.0f};
  float fog_color[4] = {0.0910f, 0.1374f, 0.1446f, 975.0f};
  float fog_scatter[4] = {0.2233f, 0.2232f, 0.2232f, 5.0f};
  float fog_params[4] = {0.00045f, 200.0f, 4000.0f, 0.9f};
  float car_light_top[4] = {0.1687f, 0.2229f, 0.2746f, 1.0f};
  float car_light_bot[4] = {0.1788f, 0.1729f, 0.1024f, 150.0f};
  // Multi-cascade sun shadow far cascade, dynamic cubemap basis, and mirror flags.
  float shadow_matrix_far[16] = {};
  float shadow_far_bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float env_basis_x[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float env_basis_y[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  float env_basis_z[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  // Paint Shop finishes, two-tone pearlescent, window tint, cosmetic damage & dirt.
  float paint_two_tone[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float paint_finish[4] = {0.0f, 0.25f, 0.0f, 0.0f};
  float damage_dirt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float damage_zones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  // Car-specific direct sun color scaled by IBLDirectScaleCar (0.6238 vs env 1.30).
  float car_sun_color[4] = {0.464f, 0.400f, 0.280f, -1.0f};
};
static_assert(sizeof(ShowroomFrame) == 704);

// Per-draw push constants.
struct ShowroomPush {
  float model[16];
  float uv0[4];
  float uv1[4];
  float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float params[4];  // material class, flags, tyre rim radius, tyre outer radius
};
static_assert(sizeof(ShowroomPush) == 128);

// A world texture ready for upload: the untiled levels of a texture record
// (WORLD-MATERIALS), sampled as sRGB when its format word says gamma.
struct WorldTextureData {
  uint32_t record = 0;
  bool srgb = false;
  cars::bake::Texture texture;
};
constexpr uint32_t kNoWorldTexture = 0xFFFFFFFFu;

// Scenery drawn around the car (carview --drive): triangles in groups of
// one flat colour each, with positions relative to an origin the caller
// folds into set_world_transform (floats stay precise far from zero). With
// materials (the textured world) the triangles also carry per corner a
// normal, the surface and lightmap UVs and the vertex colour, and per
// triangle a material.
// The decals' depth bias (ours): in float ulps of the primitive's depth and
// times its depth slope.
constexpr float kDecalBiasConstant = 64.0f, kDecalBiasSlope = 1.0f;

struct WorldTriangles {
  std::vector<std::array<float, 3>> positions;  // three per triangle
  std::vector<uint8_t> group;                   // per triangle: index into colours
  std::vector<std::array<float, 3>> colours;    // linear rgb per group
  struct Material {
    uint32_t texture = kNoWorldTexture;  // texture record of slot 0
    uint8_t blend = 0;                   // world::Blend: opaque, coverage, alpha
    uint8_t far = 0;                     // world::MeshMaterial::far: pushed back in depth
    uint8_t decal = 0;                   // world::decal_effect: drawn with a depth bias
    uint8_t layers = 0;                  // world::Layers: one, vertex blend, splat
    uint32_t layer_b = kNoWorldTexture;  // Blend_B
    uint32_t splat = kNoWorldTexture;    // Splat
    uint32_t light_map = kNoWorldTexture;   // Light_Map (night emissive spill)
    uint32_t normal_map = kNoWorldTexture;  // NormalMap (DXN/BC5)
    uint32_t ao_map = kNoWorldTexture;      // AO / AOMap
    uint32_t layer_c = kNoWorldTexture;     // Blend_C
    uint32_t spec_map = kNoWorldTexture;    // SpecularMap
    std::array<float, 4> tiling{1, 1, 1, 1};  // Blend_A's UV tiling in xy, Blend_B's (or decal noise's) in zw
    std::array<float, 2> tiling_c{1, 1};      // Blend_C's UV tiling
    std::array<float, 2> decal_noise_c4{3.5f, 8.0f};  // H_VBLND_DECAL pixel constant 4 xy
    float emissive_force = 0.0f;
    uint8_t is_water = 0;
    uint8_t anim_flag = 0;               // world::MeshMaterial::anim_flag (ANIM_FLAG*)
    uint8_t is_grass = 0;                // world::MeshMaterial::is_grass (_0x00005007)
    uint8_t is_road = 0;                 // world::MeshMaterial::is_road (H_ROAD*)
    uint8_t fade = 0;                    // world::MeshMaterial::fade: the distance cross-fade class
  };
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uv0, uv1, uv2;
  std::vector<std::array<uint8_t, 4>> vertex_colours;
  std::vector<uint32_t> material;  // per triangle: index into materials
  std::vector<std::array<float, 3>> instance;  // per triangle: its prop's origin (world::Mesh::instance)
  std::vector<Material> materials;
  // Textures not uploaded before, to upload with this scenery.
  std::vector<std::shared_ptr<const WorldTextureData>> textures;
};

// The textured world's vertex (world.vert).
struct WorldVertex {
  float position[3];
  int8_t normal[4];
  float uv0[2], uv1[2];
  uint8_t colour[4];
  // A prop's origin less the corner, in decimetres (the level is picked
  // per instance in world.vert); zero elsewhere.
  int16_t centre[4];
  // TEXCOORD2 (lightmap / AO UV) stored as IEEE-754 float16 pair, keeping the
  // stride at 48 bytes (16-byte aligned for vertex fetch).
  uint16_t uv2[2] = {0, 0};
};
static_assert(sizeof(WorldVertex) == 48);

// WorldTriangles as draw data: one vertex per corner with the face normal,
// indices, and one draw per colour. Plain CPU work with no Vulkan, so a
// streaming worker can build it (build_world) and the render thread only
// uploads it.
struct WorldGeometry {
  struct Draw {
    uint32_t first_index = 0, index_count = 0;
    ShowroomPush push{};
    uint32_t texture = kNoWorldTexture;  // textured: the texture record drawn
    uint32_t texture_b = kNoWorldTexture, splat = kNoWorldTexture;  // terrain layers / decal noise
    uint32_t light_map = kNoWorldTexture, normal_map = kNoWorldTexture;
    uint32_t ao_map = kNoWorldTexture, layer_c = kNoWorldTexture;
    uint32_t spec_map = kNoWorldTexture;
    uint8_t blend = 0;
    uint8_t decal = 0;  // drawn with the decal depth bias
    // The draw's box in the scenery's frame (tree sprites padded by their
    // corners' reach), tested against the view each frame; cull false:
    // always drawn (the sky, the flat world).
    bool cull = false;
    float box_min[3] = {0, 0, 0}, box_max[3] = {0, 0, 0};
    // The GPU-driven world (world_gpu.h): a resident draw lives in the
    // arenas (first_index and vertex_offset then address them, its indices
    // relative to its own vertices), found again across regathers by `key`.
    // `shift` (m, x and z) is what its positions need added to be relative
    // to the scenery being drawn; `fade_in` marks a draw new to the scenery,
    // dithered in with the swap; `gpu` that the culling pass draws it.
    bool resident = false, fade_in = false, gpu = false;
    int32_t vertex_offset = 0;
    uint64_t key = 0;
    uint32_t vertex_begin = 0, vertex_count = 0;  // its vertices in the staged buffer
    float shift[2] = {0, 0};
  };
  bool textured = false;
  std::vector<WorldVertex> textured_vertices;  // textured: in place of vertices
  std::vector<std::shared_ptr<const WorldTextureData>> textures;
  std::vector<cars::bake::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<Draw> draws;
  size_t triangles = 0;
};
WorldGeometry build_world(const WorldTriangles& world);

struct ShowroomOptions {
  size_t colour = 0;            // index into the car's colours (by Sequence)
  int64_t match_rgb = -1;       // when set (r x 65536 + g x 256 + b): the colour with that sRGB
  // When set: the colour with that Combo_Colors Sequence (a garage row's
  // DefaultManufacturerColorIndex, SPEC-AUTOSHOW-PAINT); none: the lowest.
  int64_t match_sequence = -1;
  bool livery_overlay = false;  // tint the livery mask magenta on the paint
  bool all_variants = false;    // draw every part variant and upgrade part
  // The Data_Car row shown (0: the baked row). Rows sharing a model can
  // differ in stock parts and factory livery (FORMAT-CARS, Stock parts).
  int64_t car_id = 0;
  // Optional game media root (for on-demand aftermarket rims, per-car tyres,
  // and upgraded brake rotors/calipers from media/brakes/).
  std::filesystem::path game_root;
  // Optional fitted visual upgrades (body kits, wing, hood, skirts, cage,
  // exhaust, aftermarket rim, upgraded brakes, ride height, track offsets,
  // tyre widths/rim sizes, camber).
  cars::VisualSetup visual;
};

class Showroom {
 public:
  explicit Showroom(Renderer& renderer) : r_(renderer) {}
  Showroom(const Showroom&) = delete;
  Showroom& operator=(const Showroom&) = delete;
  ~Showroom();

  // Reads <bake>/cars/<car>.shiftbake, its wheel and tyre bakes and
  // <bake>/shared.shiftbake, and builds the draw list. CPU only.
  bool load(const std::filesystem::path& bake_dir, const std::string& car,
            const ShowroomOptions& options, std::string& error);
  // Creates the pipelines and uploads geometry and textures; the renderer
  // must be initialised. Installs itself as the renderer's scene.
  bool upload(std::string& error);
  // The caller's camera and lighting; the stripe colour, paint finish, and damage state are the car's own.
  void set_frame(const ShowroomFrame& frame) {
    frame_ = frame;
    for (int i = 0; i < 4; ++i) {
      frame_.stripe[i] = stripe_[i];
      frame_.world_fade[i] = world_fade_[i];
      frame_.paint_two_tone[i] = paint_two_tone_[i];
      frame_.paint_finish[i] = paint_finish_[i];
      frame_.damage_dirt[i] = damage_dirt_[i];
      frame_.damage_zones[i] = damage_zones_[i];
    }
    if (custom_livery_active_) frame_.options[2] = 2.0f;
  }
  // Moves the wheels, rotors and callipers (LF, RF, LR, RR). Until it is
  // called they stand at the car's rest centres, as the showroom shows them.
  void set_wheel_poses(const WheelPose poses[4]);
  // Replaces the scenery (after upload); the GPU is idled first. Empty
  // clears it. The car-coverage mask never includes it.
  bool set_world(const WorldTriangles& world, std::string& error);
  bool set_world(WorldGeometry geometry, std::string& error);
  // Scenery staged for upload: its draws, with the vertices and indices
  // already in host-visible buffers. stage_world only creates and fills new
  // objects, so a streaming worker may call it while this thread renders;
  // begin_world then starts the GPU copy into device memory without
  // waiting for it, and the scenery in use stays drawn until poll_world
  // finds the copy done and swaps (set_world(StagedWorld) does both,
  // waiting). Staging buffers are freed when the last copy of the
  // StagedWorld goes (before the Showroom).
  struct StagedWorld {
    std::vector<WorldGeometry::Draw> draws;
    size_t triangles = 0;
    bool textured = false;
    std::vector<std::shared_ptr<const WorldTextureData>> textures;
    std::shared_ptr<Renderer::Buffer> vertices, indices;
    // The textures' levels packed in one host-visible buffer by the worker
    // (ours: uploaded in the scenery's fenced copy, not one wait each):
    // per texture, its offset and level count (0: upload it apart).
    std::shared_ptr<Renderer::Buffer> texels;
    std::vector<std::pair<VkDeviceSize, uint32_t>> texel_ranges;
    std::vector<uint32_t> texel_skips;
    // Their images, made by the worker too (per texture; empty where it
    // was already uploaded or failed, which begin_world then handles).
    // The ones begin_world does not take go with the StagedWorld.
    std::shared_ptr<std::vector<Renderer::Image>> images;
    std::shared_ptr<const void> pin_guard;
    double origin_x = 0, origin_z = 0;  // what the positions are relative to
    bool arena = false;                 // staged for the resident arenas (indices relative to each draw)
    struct TreeRaw {
      float base[3];
      float half_width;
      float up[3];
      float height;
      float uv[4];
      uint32_t rgba8;
      uint32_t texture;
    };
    std::vector<TreeRaw> trees;
  };
  // `origin_x/z`: what the geometry's positions are relative to, for the content keys of the GPU-driven world.
  bool stage_world(WorldGeometry geometry, StagedWorld& out, std::string& error, double origin_x = 0,
                   double origin_z = 0);
  bool begin_world(StagedWorld staged, std::string& error);
  // Swaps in a begun world once its copy is done (or waits, with `wait`).
  // `swapped`: whether it did.
  bool poll_world(bool wait, bool* swapped);
  bool set_world(StagedWorld staged, std::string& error);
  // The origin of the scenery being drawn (its StagedWorld's).
  double world_origin_x() const { return world_origin_[0]; }
  double world_origin_z() const { return world_origin_[1]; }
  // The scenery's positions to the car's model frame (column-major).
  void set_world_transform(const float transform[16]);
  // Dynamic unlit alpha-blended triangles in the scenery's frame (relative to
  // world_origin_x/z), e.g. the 3D driving/braking line ribbon. Empty clears it.
  void set_world_overlay(std::span<const WorldVertex> triangles);
  size_t world_triangle_count() const { return world_triangles_; }
  // The world draws of the last recorded frame: recorded and culled, and
  // the indices drawn.
  struct WorldDrawStats {
    size_t drawn = 0, culled = 0, indices = 0;
    // Draws whose texture did not upload (drawn flat with the white set) or
    // whose layer set did not (slot 0 alone).
    size_t white = 0, layer_fallback = 0;
  };
  WorldDrawStats world_draw_stats() const { return world_stats_; }
  uint32_t gpu_resident_draws() const { return gpu_world_ ? gpu_world_->resident_draws() : 0u; }
  uint32_t gpu_hiz_culled() const { return gpu_world_ ? gpu_world_->last_hiz_culled() : 0u; }
  uint32_t gpu_trees_drawn() const { return gpu_world_ ? gpu_world_->last_trees_drawn() : 0u; }
  double pipeline_compile_ms() const { return pipeline_compile_ms_; }
  uint32_t last_env_draws() const { return env_draws_; }
  uint32_t last_mirror_draws() const { return mirror_draws_; }
  uint32_t last_car_draws() const { return car_draws_; }
  uint32_t last_particle_sprites() const { return particle_alpha_count_ + particle_add_count_; }
  uint32_t last_skid_tris() const { return (skid_asphalt_count_ + skid_rut_count_) / 3u; }
  uint64_t world_staging_bytes() const { return world_staging_bytes_.load(std::memory_order_relaxed); }
  VkImageView hiz_image_view() const { return gpu_world_ ? gpu_world_->hiz_image_view() : VK_NULL_HANDLE; }
  // Frustum culling of the world's draws (on by default).
  void set_world_culling(bool on) { world_cull_ = on; }
  const ShowroomFrame& frame() const { return frame_; }
  const float* world_transform() const { return world_transform_; }
  // The props' distance cross-fade (ShowroomFrame::world_fade; band 0 off)
  // and how long a swapped-in scenery takes to dither in over the one it
  // replaces (0: swapped at once). Both ours.
  void set_world_fade(float lod, float band, float edge_start, float edge_end) {
    world_fade_[0] = lod, world_fade_[1] = band, world_fade_[2] = edge_start, world_fade_[3] = edge_end;
    for (int i = 0; i < 4; ++i) frame_.world_fade[i] = world_fade_[i];
  }
  void set_world_swap_fade(double seconds) { swap_fade_seconds_ = seconds; }
  // The colour the scene clears to, as stored in the 8-bit target (0-1).
  void set_background(float r, float g, float b);
  // The textured world's light, in the frame's space: towards the sun, up,
  // and an exposure (WORLD-MATERIALS' sunDir is in the game frame).
  void set_world_light(const float sun[3], const float up[3], float exposure);
  // Real-time sky from media/realtimesky.zip (atmosphere, Stars.bin, sun/moon, clouds).
  bool upload_sky(const world::RealTimeSkyAssets& assets, std::string& error);
  bool has_sky() const { return sky_ready_; }
  // Updates dynamic 24-hour Time-of-Day lighting, fog, and sky parameters.
  void set_tod(const world::TodState& tod, int moon_phase, float real_time_sec, float eye_game_y);
  // Updates wind animation parameters (from TrackSettings.xml) for waving flags & grass.
  void set_wind(float speed, float amplitude) { wind_speed_ = speed, wind_amplitude_ = amplitude; }
  // The direction the sun shadow is cast from (towards the light, the
  // frame's space); null: the world light's sun.
  void set_shadow_direction(const float* towards_light) {
    shadow_direction_set_ = towards_light != nullptr;
    if (towards_light)
      for (int k = 0; k < 3; ++k) shadow_direction_[k] = towards_light[k];
  }
  // Whether the car is drawn (the world reference frame leaves it out).
  void set_draw_car(bool draw) { draw_car_ = draw; }
  // Paint (SPEC-AUTOSHOW-PAINT, Manufacturer colors): the car's colour
  // rows, the drawn one, and a live preview that retints the paint draws
  // (metallic flag and stripe too; a row's own atlas needs the car rebuilt).
  const std::vector<cars::bake::Colour>& colours() const { return car_.car.colours; }
  size_t colour_index() const { return options_.colour; }
  void preview_colour(size_t index);
  // Paint Shop finishes, per-region colours (body, hood, wing, mirrors,
  // aftermarket non-chrome rims, brake calipers) & window tint.
  void set_paint_spec(const cars::CarPaintSpec& paint);
  // Uploads a composited 1024x1024 RGBA8 custom vinyl livery texture onto GPU
  // and binds it onto all car paint material descriptor sets.
  bool set_custom_livery(const uint8_t* rgba, uint32_t width, uint32_t height, std::string& error);
  // Applies cosmetic impact deformation, scratches, cracked glass, and body/tyre dirt.
  void set_damage_state(const drive::CarDamageState& dmg);
  // A prop's reach from its origin (m): the box test of LOD00 cells allows
  // an instance whose origin is inside the band but whose corners are not.
  static constexpr float kPropReach = 40;
  // Full vehicle lamp state (CAR_LAMPS.md): headlights, DRLs, tail, brake,
  // reverse, indicators, side lights, numplate, and cockpit gauge backlights.
  void set_braking(float glow);
  void set_lamp_state(const drive::CarLampState& state);
  void set_lamp_vectors(const drive::LampShaderVectors& vectors);
  // Loads kind-5 .carbin lamp points and per-car ShaderSettings.xml from disc.
  void load_car_lamps(const std::filesystem::path& game_root, const std::string& car);
  // Loads media/carlights/headlight_glow_1..7.xds + headlight_beam_dip.xds.
  bool upload_carlights(const std::filesystem::path& media_dir, std::string& error);
  // Updates world streetlamp / festival / outpost glow sprites for the active scenery.
  void set_world_glows(std::vector<world::WorldGlow> glows) { world_glows_ = std::move(glows); }
  // Optional showroom static cubemap (media/tracks/uiautoshow/staticcarcubemap.xpr).
  // Call before upload() to replace the procedural studio cubemap.
  void set_environment_xpr(std::filesystem::path xpr_path) { env_xpr_path_ = std::move(xpr_path); }
  // Living cockpit animation (steering wheel rotation, RPM/speed/fuel/boost/power
  // gauge needles, and cockpit mirror render pass enable).
  void set_cockpit_anim(const drive::CockpitAnimState& anim, bool cockpit_camera);
  // Batched/instanced soft particle atlas & skidmark pipelines (SPEC-VFX-AND-PARTICLES).
  bool init_vfx(const world::ParticleLibrary* lib, std::string& error);
  void set_particles(const std::vector<world::ParticleSprite>& alpha,
                     const std::vector<world::ParticleSprite>& additive,
                     const std::vector<std::string>& used_textures, float near_plane, bool cockpit_camera);
  void set_skidmarks(const std::vector<world::SkidVertex>& asphalt_tris,
                     const std::vector<world::SkidVertex>& rut_tris);
  // Dynamic lit 3D smashable debris mesh (game frame, rendered via world_opaque_).
  void set_debris_mesh(const std::vector<world::DebrisVertex>& tris);

  // Other cars drawn with this one's camera and light (traffic): each is a
  // Showroom loaded and uploaded as a guest (set_guest before upload: no
  // scene of its own), drawn wherever an instance names it. An instance's
  // matrix takes the guest's model frame to this car's. A guest must be
  // taken out of the instances before it is destroyed.
  void set_guest() { guest_ = true; }
  struct Instance {
    Showroom* car = nullptr;
    float model[16];
    drive::LampShaderVectors lamps{};
    bool has_lamps = false;
  };
  void set_instances(std::vector<Instance> list);
  void add_instances(const std::vector<Instance>& more);
  void bind_dynamic_env(VkImageView cube_view, VkSampler sampler);
  size_t instance_count() const { return instances_.size(); }
  size_t world_texture_count() const;
  struct WorldTextureStats {
    size_t resident_count = 0;
    size_t retired_pending = 0;
    size_t descriptor_sets_allocated = 0;
    size_t descriptor_sets_free = 0;
    size_t descriptor_pools = 0;
    uint64_t resident_bytes = 0;
    uint64_t uploads = 0;
    uint64_t evictions = 0;
    uint64_t reuploads = 0;
  };
  void set_world_texture_budget(const WorldTextureBudget& budget);
  WorldTextureBudget world_texture_budget() const;
  void set_texture_cap(modern::TextureCap cap) { texture_cap_ = cap; }
  modern::TextureCap texture_cap() const { return texture_cap_; }
  // Main thread only: reads main-thread-owned retired/pool state alongside
  // world_textures_mutex_.
  WorldTextureStats world_texture_stats() const;
  bool reserve_world_texture(uint32_t record);
  // Live texture reload (mods, texture_mods.h): uploads `texture` for a
  // resident world texture record and points every set that samples it at
  // the new image (after the GPU is idle), then frees the old one. False
  // (no change) when the record is not resident.
  bool replace_world_texture(uint32_t record, const cars::bake::Texture& texture, bool srgb, std::string& error);
  void release_prepare_pins(const std::vector<uint32_t>& records);
  // The world draws the last frame's shadow pass drew (0 without shadows).
  size_t shadow_draw_count() const { return shadow_drawn_; }
  // The traffic cars it drew.
  size_t shadow_car_count() const { return shadow_cars_; }

  // Renders the car-coverage mask (every car draw, no depth test, at the
  // colour pass's MSAA) at the current frame: 255 where any sample of the
  // pixel is covered, or with `fraction` the resolved share of covered
  // samples (0-255, 255 fully covered).
  bool render_mask(uint32_t width, uint32_t height, std::vector<uint8_t>& mask, std::string& error,
                   bool fraction = false);

  const cars::bake::CarInfo& car() const { return car_.car; }
  const std::vector<cars::bake::LiveryPlane>& livery_planes() const { return car_.livery; }
  const std::array<float, 3>& bounds_min() const { return bounds_min_; }
  const std::array<float, 3>& bounds_max() const { return bounds_max_; }
  size_t draw_count() const { return draws_.size(); }
  size_t triangle_count() const { return indices_.size() / 3; }
  size_t vertex_count() const { return vertices_.size(); }
  size_t texture_count() const { return gpu_textures_.size(); }
  const std::vector<std::string>& notes() const { return notes_; }
  const std::vector<std::string>& drawn_parts() const { return drawn_parts_; }
  const std::vector<std::string>& skipped_parts() const { return skipped_parts_; }

 private:
  struct TextureRef {
    const cars::bake::Texture* texture = nullptr;
    std::string key;
  };
  struct Draw {
    uint32_t first_index = 0, index_count = 0;
    int32_t vertex_offset = 0;
    ShowroomPush push{};
    std::array<uint32_t, 5> textures{};  // slot 0, slot 1, livery, normal_map, damage_map (UINT32_MAX = fallback)
    uint32_t descriptor = 0;
    bool glass = false;
    bool is_cover = false;    // two-pass multiply+additive detail_glass_* lamp cover
    bool brake_lamp = false;  // legacy brake lamp flag
    drive::LampChannel lamp_channel = drive::LampChannel::kNone;
    bool stripe_able = false;  // an exterior paint draw a striped colour row tints (FORMAT-CARS, Paint)
    std::string part_name;     // lower-case part name (for per-region painting: hood, wing, mirror)
    std::array<float, 3> centre{};  // car space, for sorting glass
    int wheel = -1;      // the corner a wheel, rotor or calliper draw follows
    bool spins = false;  // rims, tyres and rotors roll; callipers do not
    int8_t cockpit_role = -1;  // -1 none, 0 steering_wheel, 1 speed, 2 tach, 3 fuel, 4 boost, 5 power
    float local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // about the hub, body frame
  };
  using GpuTexture = Renderer::Image;

  // The shown row's stock parts and livery flag (null: none baked).
  const cars::bake::StockRow* stock_ = nullptr;
  void add_model(const cars::bake::Model& model, const std::vector<std::string>& parts,
                 const float transform[16], const cars::bake::Bake& pack, int axle);
  uint32_t texture_index(const std::string& slot, const cars::bake::Bake& pack, bool lod0);
  bool upload_texture(const cars::bake::Texture* texture, bool srgb, GpuTexture& out, std::string& error);
  bool create_texture_image(const cars::bake::Texture& texture, bool srgb, uint32_t levels, GpuTexture& out,
                            std::string& error, uint32_t skip = 0);
  // Makes the image, view and set of a world texture staged in `texels`
  // and records its copy into `cmd` (the scenery's fenced upload).
  // `made`: its image when the worker made it (taken), else null.
  bool begin_world_texture(const WorldTextureData& t, VkBuffer texels, VkDeviceSize offset, uint32_t levels,
                           uint32_t skip, VkCommandBuffer cmd, Renderer::Image* made, std::string& error);
  // Destroying Vulkan objects off this thread (freeing a scenery's
  // buffers takes milliseconds): `reap` runs `f` on the reaper thread now;
  // `retire` once the frame in flight is done with what `f` frees
  // (release_retired, from poll_world, hands those over).
  void reap(std::function<void()> f);
  void retire(std::function<void()> f);
  void release_retired();
  void stop_reaper();
  bool upload_environment(std::string& error);
  bool create_pipelines(std::string& error);
  void record_draws(VkCommandBuffer cmd, VkPipeline opaque, VkPipeline glass);
  void record_z_prepass(VkCommandBuffer cmd);
  void record_all_instances(VkCommandBuffer cmd, bool depth_equal);
  void record_world(VkCommandBuffer cmd);
  // The eye in a scenery's frame, and whether a cell of LOD00 copies lies
  // wholly past the LOD band (showroom.cpp).
  std::array<float, 3> eye_in_scenery(const float transform[16]) const;
  bool near_lod_beyond(const WorldGeometry::Draw& d, const std::array<float, 3>& eye) const;
  // One scenery's textured draws; swap: world.frag's dither (2 none, t in
  // 0-1 the incoming scenery, 3 + t the outgoing one).
  void record_world_textured(VkCommandBuffer cmd, const std::vector<WorldGeometry::Draw>& draws,
                             const float transform[16], float swap, bool outgoing);
  void drop_fading_world(bool now);
  // Uploads the textures not in the cache and gives each a material set.
  bool upload_world_textures(const std::vector<std::shared_ptr<const WorldTextureData>>& textures,
                             std::string& error);
  VkDescriptorSet world_set(uint32_t record) const;
  // The set binding eight uploaded world textures (albedo, layer_b, splat,
  // light_map, normal_map, ao_map, layer_c, spec_map; kNoWorldTexture uses fallbacks).
  VkDescriptorSet world_layer_set(const std::array<uint32_t, 8>& records);
  void update_frame(VkCommandBuffer cmd);
  void update_buffer(VkCommandBuffer cmd, VkBuffer buffer, const ShowroomFrame& f);
  // The sun shadow (ours): place_shadow fits the map's box around the car
  // and fills frame_'s shadow fields and shadow_frame_ (the sun's view);
  // record_shadow draws the pass (host); record_shadow_car the car's
  // opaque draws, each model matrix first taken by `m` when given.
  bool shadows_on() const;
  void place_shadow();
  void record_shadow(VkCommandBuffer cmd);
  void record_shadow_car(VkCommandBuffer cmd, const float* m);
  // The opaque and glass draws once more, each model matrix first taken by
  // `m` (a guest's instance; glass unsorted).
  void record_instance(VkCommandBuffer cmd, const float m[16], const drive::LampShaderVectors* lamps = nullptr);
  void record_instance_offscreen(VkCommandBuffer cmd, VkPipeline car_opaque, VkDescriptorSet view_frame_set,
                                 const float m[16], const drive::LampShaderVectors* lamps = nullptr);

  bool guest_ = false;
  modern::TextureCap texture_cap_ = modern::TextureCap::kFull;
  std::vector<Instance> instances_;
  Renderer& r_;
  cars::bake::Bake car_, wheel_, tyre_, tyre_textures_, shared_;
  ShowroomOptions options_;
  ShowroomFrame frame_{};
  float stripe_[4] = {1, 1, 1, 1};
  float body_to_model_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  int pending_wheel_ = -1;  // what add_model records on the draws it makes
  bool pending_spins_ = false;
  float pending_local_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::vector<cars::bake::Vertex> vertices_;
  std::vector<uint32_t> indices_;
  std::vector<Draw> draws_;
  std::vector<TextureRef> textures_;  // 0 white, 1 black, then by first use
  std::map<std::string, uint32_t> texture_by_key_;
  std::map<std::array<uint32_t, 5>, uint32_t> descriptor_by_textures_;
  std::array<float, 3> bounds_min_{}, bounds_max_{};
  bool have_bounds_ = false;
  std::vector<std::string> notes_, drawn_parts_, skipped_parts_;

  // GPU.
  std::vector<GpuTexture> gpu_textures_;
  GpuTexture environment_;
  GpuTexture shared_dirt_pattern_, shared_dirt_mask_, shared_glass_crack_;
  VkImageView bound_dyn_env_view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Renderer::Buffer vertex_buffer_, index_buffer_, frame_buffer_;
  VkDescriptorSetLayout frame_layout_ = VK_NULL_HANDLE, material_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout world_material_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout world_pipeline_layout_ = VK_NULL_HANDLE;
  GpuTexture flat_normal_tex_;  // 1x1 (128, 128, 255, 255) fallback normal map
  VkPipeline opaque_ = VK_NULL_HANDLE, opaque_after_z_ = VK_NULL_HANDLE, car_z_prepass_ = VK_NULL_HANDLE,
             glass_ = VK_NULL_HANDLE, glass_mul_ = VK_NULL_HANDLE, glass_add_ = VK_NULL_HANDLE,
             mask_ = VK_NULL_HANDLE;
  VkPipelineLayout contact_shadow_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline contact_shadow_pipe_ = VK_NULL_HANDLE;
  void record_contact_shadows(VkCommandBuffer cmd);
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet frame_set_ = VK_NULL_HANDLE;
  // The sun shadow pass: Near + Far cascades in a 2x1 atlas.
  Renderer::Buffer shadow_frame_buffer_;
  Renderer::Buffer shadow_frame_far_buffer_;
  VkDescriptorSet shadow_set_ = VK_NULL_HANDLE;
  VkDescriptorSet shadow_set_far_ = VK_NULL_HANDLE;
  ShowroomFrame shadow_frame_{};
  ShowroomFrame shadow_frame_far_{};
  VkPipeline shadow_world_ = VK_NULL_HANDLE, shadow_cutout_ = VK_NULL_HANDLE, shadow_car_ = VK_NULL_HANDLE;
  size_t shadow_drawn_ = 0, shadow_cars_ = 0;
  float shadow_direction_[3] = {0, 1, 0};
  bool shadow_direction_set_ = false;
  std::vector<VkDescriptorSet> material_sets_;
  // Scenery.
  Renderer::Buffer world_vertices_, world_indices_;
  // Dynamic world-space overlay (3D driving/braking line ribbon).
  static constexpr size_t kMaxOverlayVertices = 1365;  // <= 65536 B for vkCmdUpdateBuffer
  Renderer::Buffer overlay_vertices_;
  std::vector<WorldVertex> overlay_cpu_;
  uint32_t overlay_count_ = 0;
  bool overlay_dirty_ = false;
  // The scenery being swapped out, drawn dithered under the new one until
  // the swap fade ends (ours).
  struct FadingWorld {
    Renderer::Buffer vertices, indices;
    std::vector<WorldGeometry::Draw> draws;
    bool arena = false;          // its draws are resident (their references are held until it goes)
    double origin[2] = {0, 0};
  };
  float world_fade_[4] = {0, 0, 0, 0};  // set_world_fade's, kept over set_frame
  bool fading_on_ = false;
  FadingWorld fading_;
  std::chrono::steady_clock::time_point fade_start_{};
  double swap_fade_seconds_ = 0;
  std::vector<WorldGeometry::Draw> world_draws_;
  std::vector<StagedWorld::TreeRaw> world_trees_;
  float prev_vp_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool have_prev_vp_ = false;
  bool hiz_enabled_ = true;
  // The GPU-driven world: arenas, bindless textures, culling (world_gpu.h).
  std::unique_ptr<WorldGpu> gpu_world_;
  bool arena_on() const;  // the arenas hold the scenery (else the monolithic buffers)
  uint64_t world_generation_ = 0;
  void world_bind_buffers(VkCommandBuffer cmd, bool fading);
  void world_draw_model(const WorldGeometry::Draw& d, const float transform[16], float out[16]) const;
  void world_release_draws(std::vector<WorldGeometry::Draw>& draws);
  bool build_gpu_table(std::string& error);
  void record_world_cull(VkCommandBuffer cmd);
  bool place_world_arena(StagedWorld& st, std::vector<WorldGpu::Copy>& vertex_copies,
                         std::vector<WorldGpu::Copy>& index_copies, std::string& error);
  size_t world_triangles_ = 0;
  size_t gpu_table_white_ = 0, gpu_table_layer_fallback_ = 0;
  WorldDrawStats world_stats_;
  bool world_cull_ = true;
  float world_transform_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double world_origin_[2] = {0, 0};
  struct PendingWorld {
    Renderer::Buffer vertices, indices;
    StagedWorld staged;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
  };
  std::unique_ptr<PendingWorld> pending_world_;
  void drop_pending_world();
  // The textured world.
  bool world_textured_ = false;
  bool draw_car_ = true;
  VkPipeline world_opaque_ = VK_NULL_HANDLE, world_opaque_after_z_ = VK_NULL_HANDLE,
             world_z_prepass_ = VK_NULL_HANDLE, world_coverage_ = VK_NULL_HANDLE,
             world_alpha_ = VK_NULL_HANDLE;
  std::map<uint32_t, GpuTexture> world_textures_;     // by texture record
  // Held for writes to world_textures_ and for the worker's reads.
  mutable std::mutex world_textures_mutex_;
  std::thread reaper_;
  std::mutex reap_mutex_;
  std::condition_variable reap_cv_;
  std::deque<std::function<void()>> reap_queue_;
  bool reap_stop_ = false;
  std::vector<std::pair<uint64_t, std::function<void()>>> retired_;  // main thread only
  std::map<uint32_t, VkDescriptorSet> world_sets_;   // by texture record
  // Sets for multi-texture world draws: bindings 0..7.
  std::map<std::array<uint32_t, 8>, VkDescriptorSet> world_layer_sets_;
  VkDescriptorSet world_white_set_ = VK_NULL_HANDLE;  // untextured materials
  std::vector<VkDescriptorPool> world_pools_;
  uint32_t world_pool_left_ = 0;
  float world_sun_[4] = {0.3848f, 0.8754f, 0.2926f, 1}, world_up_[4] = {0, 1, 0, 1};
  bool world_lit_ = false;  // set_world_light was called: light the car by the world's sun and sky
  // Dynamic 24-hour Time-of-Day & real-time sky (realtimesky.zip + Stars.bin).
  bool tod_active_ = false;
  world::TodState tod_state_{};
  int tod_moon_phase_ = 2;
  float tod_real_time_ = 0.0f;
  float tod_eye_game_y_ = 150.0f;
  bool sky_ready_ = false;
  bool sky_has_textures_ = false;
  struct SkyUniforms {
    float cam_fwd[4];
    float cam_right[4];
    float cam_up[4];
    float sun_dir[4];
    float sun_obj_dir[4];
    float haze_dir[4];
    float atmo_top[4];
    float atmo_bottom[4];
    float atmo_haze[4];
    float atmo_haze_p[4];
    float sun_obj_col[4];
    float far_dense[4];
    float far_thin[4];
    float far_params[4];
    float far_back_dense[4];
    float far_back_thin[4];
    float far_scroll[4];
    float close_top_col[4];
    float close_bot_col[4];
    float close_back_col[4];
    float close_dense_col[4];
    float close_thin_col[4];
    float close_intens[4];
    float close_masks[4];
    float close_layers[4];
    float fog_color[4];
    float fog_scatter[4];
    float fog_sky[4];
  };
  static_assert(sizeof(SkyUniforms) == 28 * 16);
  Renderer::Buffer sky_ubo_[2];  // mode 0 (atmosphere), mode 1 (disc+clouds+fog)
  Renderer::Buffer star_vertices_;
  uint32_t star_count_ = 0;
  GpuTexture sky_sun_, sky_moons_[5], sky_dither_;
  GpuTexture sky_far_f_, sky_far_b_, sky_far_mask_;
  GpuTexture sky_close_ad_f_, sky_close_ad_b_, sky_close_am_f_, sky_close_am_b_, sky_close_tb_f_, sky_close_tb_b_;
  VkDescriptorSetLayout sky_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout sky_pipe_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout stars_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline sky_pipe_base_ = VK_NULL_HANDLE, sky_pipe_layers_ = VK_NULL_HANDLE, stars_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool sky_pool_ = VK_NULL_HANDLE;
  // 6 sets per mode (sun + 5 moon phases) -> 12 sets total; index = mode * 6 + disc_idx (0=sun, 1..5=moon0..4).
  VkDescriptorSet sky_sets_[12] = {};
  void record_sky_before(VkCommandBuffer cmd);
  void record_sky_draw(VkCommandBuffer cmd);
  void apply_tod_to_frame();
  // Car lamps, headlight road projection & additive glow sprites (CAR_LAMPS.md).
  struct HeadlightBeamGpu {
    float proj[16];
    float origin_range[4];
    float dir_power[4];
    float color[4];
  };
  struct HeadlightUbo {
    float count_night[4];
    HeadlightBeamGpu beams[16];
  };
  static_assert(sizeof(HeadlightUbo) == 16 + 16 * 112);
  struct GlowSpriteGpu {
    float pos_radius[4];
    float dir_inner[4];
    float color_outer[4];
    float extra[4];
  };
  static_assert(sizeof(GlowSpriteGpu) == 64);
  static constexpr uint32_t kMaxGlowSprites = 1024;
  std::vector<fh1::CarbinLampPoint> lamp_points_;
  drive::LampColorTable lamp_colors_;
  drive::LampShaderVectors lamp_vectors_{};
  bool lamp_state_active_ = false;
  std::vector<world::WorldGlow> world_glows_;
  GpuTexture headlight_cookie_;
  Renderer::Buffer headlights_ubo_;
  GpuTexture glow_tex_[7];
  VkDescriptorSetLayout glow_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout glow_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline glow_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool glow_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet glow_set_ = VK_NULL_HANDLE;
  Renderer::Buffer glow_buffer_;
  uint32_t glow_count_ = 0;
  bool carlights_ready_ = false;
  void record_lights_before(VkCommandBuffer cmd);
  void record_glows_draw(VkCommandBuffer cmd);
  void update_frame_cookie_descriptor();
  // Showroom XPR2 cubemap, live 256x256 dynamic drive reflection cubemap, and 1024x256 3-band mirror pass.
  std::filesystem::path env_xpr_path_;
  static constexpr uint32_t kDynEnvSize = 256;
  static constexpr uint32_t kDynEnvMips = 9;
  static constexpr uint32_t kMirrorWidth = 1024;
  static constexpr uint32_t kMirrorHeight = 256;
  GpuTexture dyn_env_;
  VkImageView dyn_env_faces_[6] = {};
  VkImageView dyn_env_mip0_cube_ = VK_NULL_HANDLE;
  VkImageView dyn_env_mip_faces_[kDynEnvMips][6] = {};
  GpuTexture dyn_env_depth_;
  GpuTexture mirror_tex_;
  GpuTexture mirror_depth_;
  Renderer::Buffer env_frame_buffer_;
  VkDescriptorSet env_frame_set_ = VK_NULL_HANDLE;
  Renderer::Buffer env_sky_ubo_[2];
  VkDescriptorPool env_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet env_sky_sets_[12] = {};
  VkPipeline env_world_opaque_ = VK_NULL_HANDLE;
  VkPipeline env_world_coverage_ = VK_NULL_HANDLE;
  VkPipeline env_car_opaque_ = VK_NULL_HANDLE;
  VkPipeline env_sky_pipe_base_ = VK_NULL_HANDLE;
  VkPipeline env_sky_pipe_layers_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout env_prefilter_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout env_prefilter_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline env_prefilter_pipe_ = VK_NULL_HANDLE;
  VkDescriptorSet env_prefilter_set_ = VK_NULL_HANDLE;
  bool dyn_env_ready_ = false;
  bool dyn_env_initialized_ = false;
  bool dyn_env_bound_ = false;
  bool mirror_enabled_ = false;
  bool mirror_valid_ = false;
  bool init_dynamic_reflections(std::string& error);
  void record_env_and_mirror(VkCommandBuffer cmd);
  void record_offscreen_view(VkCommandBuffer cmd, VkImageView color_view, VkImageView depth_view,
                             uint32_t w, uint32_t h, const ShowroomFrame& view_frame, bool include_cars,
                             uint32_t vx = 0, uint32_t vy = 0, uint32_t vw = 0, uint32_t vh = 0,
                             bool clear_target = true, uint32_t* draw_counter = nullptr);
  void fill_sky_uniforms(const ShowroomFrame& view_frame, SkyUniforms& u) const;
  // Batched soft particles + terrain-conforming skidmarks (SPEC-VFX-AND-PARTICLES).
  struct ParticleGpu {
    float pos_sx[4];
    float vel_sy[4];
    float uv_rect[4];
    float color[4];
    float light[4];
    float extra[4];
  };
  static_assert(sizeof(ParticleGpu) == 96);
  struct SkidGpuVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t rgba[4];
  };
  static_assert(sizeof(SkidGpuVertex) == 36);
  static constexpr uint32_t kMaxParticleGpu = 2400;
  static constexpr uint32_t kMaxSkidVerts = 24000;
  static constexpr uint32_t kParticleAtlasGrid = 8;
  static constexpr uint32_t kParticleAtlasCell = 256;
  static constexpr uint32_t kParticleAtlasSize = kParticleAtlasGrid * kParticleAtlasCell;  // 2048
  bool vfx_ready_ = false;
  GpuTexture particle_atlas_;
  GpuTexture skid_asphalt_tex_;
  GpuTexture skid_rut_tex_;
  VkSampler particle_sampler_ = VK_NULL_HANDLE;
  VkSampler skid_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout particle_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout particle_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline particle_alpha_pipe_ = VK_NULL_HANDLE;
  VkPipeline particle_add_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool particle_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet particle_set_ = VK_NULL_HANDLE;
  VkImageView particle_bound_depth_ = VK_NULL_HANDLE;
  // A vertex/instance buffer the CPU rewrites each frame, one region per
  // frame in flight: the setters write `cpu` (the newest content) and the
  // draw copies it into its slot's region when that region is out of date.
  struct DynBuffer {
    Renderer::Buffer buf;         // kFramesInFlight regions of `cap` bytes
    void* gpu = nullptr;          // its mapping
    std::vector<uint8_t> cpu;     // `cap` bytes
    VkDeviceSize cap = 0;
    uint64_t ver = 1;             // bumped when `cpu` changes
    uint64_t slot_ver[Renderer::kFramesInFlight] = {};
    bool ready() const { return gpu != nullptr && !cpu.empty(); }
    void* data() { return cpu.data(); }
  };
  bool create_dyn_buffer(DynBuffer& d, VkDeviceSize cap, std::string& error,
                         VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  void destroy_dyn_buffer(DynBuffer& d);
  // Copies the content into this frame's region if needed; returns the
  // region's byte offset to bind.
  VkDeviceSize dyn_sync(DynBuffer& d, VkDeviceSize used_bytes);
  static constexpr uint32_t kMaxCarInstances = 256;
  struct CarInstanceGpu {
    float model[16];
    float stripe[4];
    float lamp_rgb[30][4];
    float pad[4];
  };
  static_assert(sizeof(CarInstanceGpu) == 576);
  DynBuffer car_inst_buf_;
  Renderer::Buffer empty_inst_ssbo_;
  DynBuffer particle_buf_;
  uint32_t particle_alpha_count_ = 0;
  uint32_t particle_add_count_ = 0;
  float particle_near_plane_ = 0.25f;
  bool particle_cockpit_ = false;
  std::unordered_map<std::string, uint16_t> atlas_tex_slot_;  // lower-case .xds -> atlas cell [0..63]
  VkDescriptorSetLayout skid_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout skid_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline skid_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool skid_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet skid_sets_[2] = {};
  DynBuffer skid_asphalt_buf_;
  DynBuffer skid_rut_buf_;
  uint32_t skid_asphalt_count_ = 0;
  uint32_t skid_rut_count_ = 0;
  static constexpr uint32_t kMaxDebrisVerts = 65536;  // smashable debris + animated world props/grass
  DynBuffer debris_buf_;
  uint32_t debris_count_ = 0;
  static constexpr uint32_t kMaxDriverVerts = 2048;   // animated 3D seated driver + steering hands
  DynBuffer driver_buf_;
  uint32_t driver_count_ = 0;
  float driver_wheel_rad_ = 0.0f;
  bool cockpit_camera_active_ = false;
  float wind_speed_ = 1.0f;
  float wind_amplitude_ = 0.15f;
  void update_driver_mesh(float wheel_rad);
  void record_driver_draw(VkCommandBuffer cmd);
  void record_skidmarks_draw(VkCommandBuffer cmd);
  void record_debris_draw(VkCommandBuffer cmd);
  void record_particles_draw(VkCommandBuffer cmd);
  // Living cockpit animation rigs (steering wheel + 5 analog gauge needles).
  struct CockpitRig {
    bool valid = false;
    std::array<float, 3> pivot{0, 0, 0};
    std::array<float, 3> axis{0, 0, 1};
    float ang_mid = 0.0f;
    float ang_max = 0.0f;
  };
  CockpitRig cockpit_rigs_[6]{};  // 0 steering_wheel, 1 speed, 2 tach, 3 fuel, 4 boost, 5 power
  bool allocate_world_set(VkDescriptorSet& out, std::string& error);
  bool create_world_single_set(VkImageView view, VkDescriptorSet& out, std::string& error);
  void evict_world_textures();
  WorldTextureLru world_lru_;
  bool world_budget_explicit_ = false;
  uint64_t world_frame_ = 0;
  std::unordered_map<uint32_t, size_t> prepare_pins_;
  std::vector<std::pair<uint64_t, std::vector<VkDescriptorSet>>> retired_sets_;  // main thread only
  std::vector<VkDescriptorSet> free_world_sets_;
  size_t world_sets_allocated_ = 0;
  // Paint Shop finish/two-tone/tint, custom 1024^2 livery, and cosmetic damage/dirt state.
  float paint_two_tone_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float paint_finish_[4] = {0.0f, 0.25f, 0.0f, 0.0f};
  float damage_dirt_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float damage_zones_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool custom_livery_active_ = false;
  GpuTexture custom_livery_img_;
  // Benchmark telemetry counters.
  double pipeline_compile_ms_ = 0.0;
  uint32_t env_draws_ = 0;
  uint32_t mirror_draws_ = 0;
  uint32_t car_draws_ = 0;
  std::atomic<uint64_t> world_staging_bytes_{0};
};

}  // namespace carview
