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
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <volk.h>

#include "cars/bake_format.h"
#include "renderer.h"

namespace carview {

// The frame's uniform block (showroom.vert / showroom.frag, set 0 binding 0).
struct ShowroomFrame {
  float view_projection[16];  // car space to Vulkan clip, column-major
  float eye[4];               // camera position in car space
  float key_light[4];         // towards the light, intensity
  float fill_light[4];
  float options[4];  // exposure, environment intensity, livery overlay, metallic paint's metal share
  float tint[4];     // light colour (linear rgb) for diffuse light: environment and both lights
  float reflect_tint[4];  // colour of environment reflections (linear rgb)
};
static_assert(sizeof(ShowroomFrame) == 160);

// Per-draw push constants.
struct ShowroomPush {
  float model[16];
  float uv0[4];
  float uv1[4];
  float tint[4];
  float params[4];  // material class, flags, tyre rim radius, tyre outer radius
};
static_assert(sizeof(ShowroomPush) == 128);

struct ShowroomOptions {
  size_t colour = 0;            // index into the car's colours (by Sequence)
  int64_t match_rgb = -1;       // when set (r x 65536 + g x 256 + b): the colour with that sRGB
  bool livery_overlay = false;  // tint the livery mask magenta on the paint
  bool all_variants = false;    // draw every part variant and upgrade part
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
  void set_frame(const ShowroomFrame& frame) { frame_ = frame; }
  // The colour the scene clears to, as stored in the 8-bit target (0-1).
  void set_background(float r, float g, float b);

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
  };
  using GpuTexture = Renderer::Image;

  void add_model(const cars::bake::Model& model, const std::vector<std::string>& parts,
                 const float transform[16], const cars::bake::Bake& pack, int axle, bool tyre_fit);
  uint32_t texture_index(const std::string& slot, const cars::bake::Bake& pack, bool lod0);
  bool upload_texture(const cars::bake::Texture* texture, bool srgb, GpuTexture& out, std::string& error);
  bool upload_environment(std::string& error);
  bool create_pipelines(std::string& error);
  void record_draws(VkCommandBuffer cmd, VkPipeline opaque, VkPipeline glass);
  void update_frame(VkCommandBuffer cmd);

  Renderer& r_;
  cars::bake::Bake car_, wheel_, tyre_, tyre_textures_, shared_;
  ShowroomOptions options_;
  ShowroomFrame frame_{};
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
  std::vector<VkDescriptorSet> material_sets_;
};

}  // namespace carview
