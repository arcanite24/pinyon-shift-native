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
#include <vector>

#include <volk.h>

#include "cars/bake_format.h"
#include "renderer.h"

namespace carview {

// One wheel's pose for set_wheel_poses, in the body frame (FORMAT-CARS:
// +X right, +Y up, +Z forward): the hub centre, the steer angle about y
// (positive towards +x) and the rolling angle about x (positive rolling
// forward).
struct WheelPose {
  float centre[3] = {0, 0, 0};
  float steer = 0, spin = 0;
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
};
static_assert(sizeof(ShowroomFrame) == 352);

// Per-draw push constants.
struct ShowroomPush {
  float model[16];
  float uv0[4];
  float uv1[4];
  float tint[4];
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
    std::array<float, 4> tiling{1, 1, 1, 1};  // Blend_A's UV tiling in xy, Blend_B's in zw
    uint8_t fade = 0;                    // world::MeshMaterial::fade: the distance cross-fade class
  };
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uv0, uv1;
  std::vector<std::array<uint8_t, 4>> vertex_colours;
  std::vector<uint32_t> material;  // per triangle: index into materials
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
};
static_assert(sizeof(WorldVertex) == 36);

// WorldTriangles as draw data: one vertex per corner with the face normal,
// indices, and one draw per colour. Plain CPU work with no Vulkan, so a
// streaming worker can build it (build_world) and the render thread only
// uploads it.
struct WorldGeometry {
  struct Draw {
    uint32_t first_index = 0, index_count = 0;
    ShowroomPush push{};
    uint32_t texture = kNoWorldTexture;  // textured: the texture record drawn
    uint32_t texture_b = kNoWorldTexture, splat = kNoWorldTexture;  // terrain layers
    uint8_t blend = 0;
    uint8_t decal = 0;  // drawn with the decal depth bias
    // The draw's box in the scenery's frame (tree sprites padded by their
    // corners' reach), tested against the view each frame; cull false:
    // always drawn (the sky, the flat world).
    bool cull = false;
    float box_min[3] = {0, 0, 0}, box_max[3] = {0, 0, 0};
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
  bool livery_overlay = false;  // tint the livery mask magenta on the paint
  bool all_variants = false;    // draw every part variant and upgrade part
  // The Data_Car row shown (0: the baked row). Rows sharing a model can
  // differ in stock parts and factory livery (FORMAT-CARS, Stock parts).
  int64_t car_id = 0;
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
  // The caller's camera and lighting; the stripe colour is the car's own.
  void set_frame(const ShowroomFrame& frame) {
    frame_ = frame;
    for (int i = 0; i < 4; ++i) frame_.stripe[i] = stripe_[i];
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
    // Their images, made by the worker too (per texture; empty where it
    // was already uploaded or failed, which begin_world then handles).
    // The ones begin_world does not take go with the StagedWorld.
    std::shared_ptr<std::vector<Renderer::Image>> images;
    double origin_x = 0, origin_z = 0;  // what the positions are relative to
  };
  bool stage_world(WorldGeometry geometry, StagedWorld& out, std::string& error);
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
  size_t world_triangle_count() const { return world_triangles_; }
  // The world draws of the last recorded frame: recorded and culled, and
  // the indices drawn.
  struct WorldDrawStats {
    size_t drawn = 0, culled = 0, indices = 0;
  };
  WorldDrawStats world_draw_stats() const { return world_stats_; }
  // Frustum culling of the world's draws (on by default).
  void set_world_culling(bool on) { world_cull_ = on; }
  // The props' distance cross-fade (ShowroomFrame::world_fade; band 0 off)
  // and how long a swapped-in scenery takes to dither in over the one it
  // replaces (0: swapped at once). Both ours.
  void set_world_fade(float lod, float band, float edge_start, float edge_end) {
    frame_.world_fade[0] = lod, frame_.world_fade[1] = band;
    frame_.world_fade[2] = edge_start, frame_.world_fade[3] = edge_end;
  }
  void set_world_swap_fade(double seconds) { swap_fade_seconds_ = seconds; }
  // The colour the scene clears to, as stored in the 8-bit target (0-1).
  void set_background(float r, float g, float b);
  // The textured world's light, in the frame's space: towards the sun, up,
  // and an exposure (WORLD-MATERIALS' sunDir is in the game frame).
  void set_world_light(const float sun[3], const float up[3], float exposure);
  // The direction the sun shadow is cast from (towards the light, the
  // frame's space); null: the world light's sun.
  void set_shadow_direction(const float* towards_light) {
    shadow_direction_set_ = towards_light != nullptr;
    if (towards_light)
      for (int k = 0; k < 3; ++k) shadow_direction_[k] = towards_light[k];
  }
  // Whether the car is drawn (the world reference frame leaves it out).
  void set_draw_car(bool draw) { draw_car_ = draw; }

  // Other cars drawn with this one's camera and light (traffic): each is a
  // Showroom loaded and uploaded as a guest (set_guest before upload: no
  // scene of its own), drawn wherever an instance names it. An instance's
  // matrix takes the guest's model frame to this car's. A guest must be
  // taken out of the instances before it is destroyed.
  void set_guest() { guest_ = true; }
  struct Instance {
    Showroom* car = nullptr;
    float model[16];
  };
  void set_instances(std::vector<Instance> list) { instances_ = std::move(list); }
  size_t instance_count() const { return instances_.size(); }
  size_t world_texture_count() const { return world_textures_.size(); }
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
    std::array<uint32_t, 3> textures{};  // slot 0, slot 1, livery (gpu_textures_ indices)
    uint32_t descriptor = 0;
    bool glass = false;
    std::array<float, 3> centre{};  // car space, for sorting glass
    int wheel = -1;      // the corner a wheel, rotor or calliper draw follows
    bool spins = false;  // rims, tyres and rotors roll; callipers do not
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
                            std::string& error);
  // Makes the image, view and set of a world texture staged in `texels`
  // and records its copy into `cmd` (the scenery's fenced upload).
  // `made`: its image when the worker made it (taken), else null.
  bool begin_world_texture(const WorldTextureData& t, VkBuffer texels, VkDeviceSize offset, uint32_t levels,
                           VkCommandBuffer cmd, Renderer::Image* made, std::string& error);
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
  void record_world(VkCommandBuffer cmd);
  // One scenery's textured draws; swap: world.frag's dither (2 none, t in
  // 0-1 the incoming scenery, 3 + t the outgoing one).
  void record_world_textured(VkCommandBuffer cmd, const std::vector<WorldGeometry::Draw>& draws,
                             const float transform[16], float swap, bool outgoing);
  void drop_fading_world(bool now);
  // Uploads the textures not in the cache and gives each a material set.
  bool upload_world_textures(const std::vector<std::shared_ptr<const WorldTextureData>>& textures,
                             std::string& error);
  VkDescriptorSet world_set(uint32_t record) const;
  // The set binding three uploaded textures (kNoWorldTexture: white), made
  // on first use; null when one of them did not upload.
  VkDescriptorSet world_layer_set(const std::array<uint32_t, 3>& records);
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
  void record_instance(VkCommandBuffer cmd, const float m[16]);

  bool guest_ = false;
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
  std::map<std::array<uint32_t, 3>, uint32_t> descriptor_by_textures_;
  std::array<float, 3> bounds_min_{}, bounds_max_{};
  bool have_bounds_ = false;
  std::vector<std::string> notes_, drawn_parts_, skipped_parts_;

  // GPU.
  std::vector<GpuTexture> gpu_textures_;
  GpuTexture environment_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Renderer::Buffer vertex_buffer_, index_buffer_, frame_buffer_;
  VkDescriptorSetLayout frame_layout_ = VK_NULL_HANDLE, material_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline opaque_ = VK_NULL_HANDLE, glass_ = VK_NULL_HANDLE, mask_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet frame_set_ = VK_NULL_HANDLE;
  // The sun shadow pass: its frame block and set, and depth-only pipelines.
  Renderer::Buffer shadow_frame_buffer_;
  VkDescriptorSet shadow_set_ = VK_NULL_HANDLE;
  ShowroomFrame shadow_frame_{};
  VkPipeline shadow_world_ = VK_NULL_HANDLE, shadow_cutout_ = VK_NULL_HANDLE, shadow_car_ = VK_NULL_HANDLE;
  size_t shadow_drawn_ = 0, shadow_cars_ = 0;
  float shadow_direction_[3] = {0, 1, 0};
  bool shadow_direction_set_ = false;
  std::vector<VkDescriptorSet> material_sets_;
  // Scenery.
  Renderer::Buffer world_vertices_, world_indices_;
  // The scenery being swapped out, drawn dithered under the new one until
  // the swap fade ends (ours).
  struct FadingWorld {
    Renderer::Buffer vertices, indices;
    std::vector<WorldGeometry::Draw> draws;
    double origin[2] = {0, 0};
  };
  bool fading_on_ = false;
  FadingWorld fading_;
  std::chrono::steady_clock::time_point fade_start_{};
  double swap_fade_seconds_ = 0;
  std::vector<WorldGeometry::Draw> world_draws_;
  size_t world_triangles_ = 0;
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
  VkPipeline world_opaque_ = VK_NULL_HANDLE, world_coverage_ = VK_NULL_HANDLE, world_alpha_ = VK_NULL_HANDLE;
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
  // Sets for terrain draws: slot 0, Blend_B and Splat (bindings 0 to 2).
  std::map<std::array<uint32_t, 3>, VkDescriptorSet> world_layer_sets_;
  VkDescriptorSet world_white_set_ = VK_NULL_HANDLE;  // untextured materials
  std::vector<VkDescriptorPool> world_pools_;
  uint32_t world_pool_left_ = 0;
  float world_sun_[4] = {0.3848f, 0.8754f, 0.2926f, 1}, world_up_[4] = {0, 1, 0, 1};
  bool allocate_world_set(VkDescriptorSet& out, std::string& error);
};

}  // namespace carview
