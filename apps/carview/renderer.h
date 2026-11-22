// A minimal Vulkan 1.3 renderer for carview: dynamic rendering, one
// pipeline, a depth buffer, MSAA and one texture. It renders either to an
// SDL window's swapchain or offscreen (headless, no window system at all).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <volk.h>

VK_DEFINE_HANDLE(VmaAllocator);
VK_DEFINE_HANDLE(VmaAllocation);

#include "ui.h"

struct SDL_Window;

namespace carview {

struct CarModel;

struct FrameConstants {
  float view_projection[16];  // column-major
  float light_direction[4];
  float paint[4];
  float eye[4];
};
static_assert(sizeof(FrameConstants) == 112);

class Showroom;

// Lets another scene (the showroom) record its own draws in place of the
// single car pipeline: `before` runs outside rendering (buffer updates),
// `draw` inside it with the viewport and scissor set.
struct SceneHooks {
  std::function<void(VkCommandBuffer)> before;
  std::function<void(VkCommandBuffer)> draw;
  float clear[4] = {0.16f, 0.18f, 0.21f, 1.0f};
};

// The drive's post chain (SHOWROOM_LIGHTING, Post chain): the scene is
// drawn as linear HDR (RGBA16F), then a composite tone-maps it into the
// output. The defaults are Colorado's TrackSettings.xml (filmicTone,
// DesaturateAmount, DesatLuminosity, bloom cutoff and scale).
struct PostSettings {
  bool enabled = false;
  bool bloom = true;                                              // ours: a quarter-size bright pass and blur
  float hable[6] = {0.700001f, 0.038f, 0.634f, 0.220001f, 0.003f, 0.107f};  // A, B, C, D, E, F
  float white = 1.0f;
  float exposure = 0.31f;  // times the scene before the curve
  float desaturate = 0.9f;  // the colour's share; the rest is its luminance
  float luminance[3] = {0.2126f, 0.7152f, 0.0722f};
  float bloom_cutoff = 1.0f, bloom_scale = 1.0f;
  // Ours, not the spec's (WORLD_MATERIALS leaves the grade open): a linear
  // gain after the curve, fitted to the game's frames of the hills.
  float balance[3] = {1.0f, 1.0f, 1.0f};
};

// The drive's sun shadow (ours): one orthographic cascade around the car,
// drawn from the sun each frame. size 0: none (a 1x1 map stays bound).
struct ShadowSettings {
  uint32_t size = 0;      // the map's side in texels
  float radius = 60.0f;   // half the box's side, metres
  uint32_t taps = 4;      // 1: one filtered compare; 4: four
  float strength = 0.35f;  // the sun's share left in shadow (TrackSettings ShadowColor, read as ours)
};

class Renderer {
 public:
  struct Options {
    SDL_Window* window = nullptr;  // null: headless
    bool validation = false;
    uint32_t msaa = 4;
    PostSettings post;
    // Anisotropic filtering of the scene's textures, clamped to what the
    // device offers (1: off). Set by the preset (ours).
    float anisotropy = 1.0f;
    ShadowSettings shadow;
    // Reversed depth (ours, the drive): the scene clears depth to 0, tests
    // GREATER(_OR_EQUAL), and the camera's projection maps the near plane to
    // 1 and infinity to 0, so the float depth keeps its precision far out.
    bool reverse_z = false;
  };

  Renderer() = default;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  ~Renderer();

  bool init(const Options& options, std::string& error);
  bool upload(const CarModel& model, std::string& error);
  // Replaces the vertices and indices of an uploaded model; the texture stays.
  bool replace_geometry(const CarModel& model, std::string& error);

  // Headless: renders one frame of `width` x `height` and reads it back as
  // RGBA8, and with `depth` also the depth buffer (one sample, D32_SFLOAT).
  bool render_offscreen(const FrameConstants& constants, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& rgba, std::string& error,
                        std::vector<float>* depth = nullptr);
  // Headless benchmark: renders one frame into a kept target without
  // reading it back. `record_ms` is the CPU time to record the commands,
  // `gpu_ms` the GPU time between timestamps at the start and end of the
  // frame (negative when the queue has no timestamps).
  bool render_timed(const FrameConstants& constants, uint32_t width, uint32_t height, double& record_ms,
                    double& gpu_ms, std::string& error);
  // render_timed's split: the GPU time of each stage the scene marked
  // (mark), in order, from the last timed frame.
  const std::vector<std::pair<std::string, double>>& stage_ms() const { return stage_ms_; }
  // Ends a stage of a timed frame at this point of `cmd` (nothing otherwise).
  void mark(VkCommandBuffer cmd, const char* label);
  // The format the scene's pipelines draw into: the output's, or RGBA16F
  // under the post chain.
  VkFormat scene_format() const { return options_.post.enabled ? VK_FORMAT_R16G16B16A16_SFLOAT : color_format_; }
  bool post() const { return options_.post.enabled; }
  bool reverse_z() const { return options_.reverse_z; }
  // The MSAA sample count in use (1 when off).
  uint32_t sample_count() const { return uint32_t(samples_); }
  // The anisotropy the scene's samplers use: options.anisotropy within the
  // device's limit, 1 when the device has none.
  float sampler_anisotropy() const { return anisotropy_; }
  // Windowed: renders and presents one frame. Returns false on a fatal error.
  bool draw_frame(const FrameConstants& constants, std::string& error);

  // With hooks set, frames record the hooked scene; upload() is not needed.
  void set_scene(SceneHooks hooks) { scene_ = std::move(hooks); }

  // An opaque panel drawn on the CPU (RGBA8, already sRGB-encoded) and
  // copied over every following frame with its top-left corner at (x, y)
  // pixels; a negative x or y counts from the right or bottom edge. An
  // empty panel removes it. Frames it does not fit in are drawn without it.
  // Up to kOverlayLayers panels (the HUD, the minimap, the pause map), each
  // its own layer; they must not overlap. A layer is copied to the GPU's
  // buffer only when set again.
  static constexpr int kOverlayLayers = 4;
  void set_overlay(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, int32_t x, int32_t y, int layer = 0);

  // The UI layer (ui.h): shapes blended over every following frame after
  // the post chain and the overlay panels, until set again; empty: none.
  void set_ui(std::span<const ui::Instance> items);
  // Copies `rgba` (w x h, sRGB-encoded RGBA8) into the UI atlas (kUiAtlas
  // square) at (x, y), for kImage shapes; applied with the next frame.
  static constexpr uint32_t kUiAtlas = 2048;
  void ui_atlas_put(uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::span<const uint8_t> rgba);

  const std::string& device_name() const { return device_name_; }
  uint32_t samples() const { return uint32_t(samples_); }
  uint32_t validation_messages() const { return validation_messages_; }

  struct VramStats {
    uint32_t block_count = 0;
    uint32_t allocation_count = 0;
    uint64_t block_bytes = 0;
    uint64_t allocation_bytes = 0;
    uint64_t device_local_usage = 0;
    uint64_t device_local_budget = 0;
  };
  VramStats vram_stats() const;
  VramStats vram_detailed_stats() const;
  VkDeviceSize device_local_bytes() const;

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
  };

  bool create_instance(std::string& error);
  bool pick_device(std::string& error);
  bool create_device(std::string& error);
  bool create_pipeline(VkFormat color_format, std::string& error);
  bool create_targets(uint32_t width, uint32_t height, std::string& error);
  bool create_post(std::string& error);  // the post pipelines and sets (once)
  // The shadow map (options_.shadow.size, or 1x1 without shadows) and its
  // comparison sampler, made once; left in DEPTH_STENCIL_READ_ONLY_OPTIMAL.
  bool ensure_shadow_map(std::string& error);
  void record_post(VkCommandBuffer cmd, VkImageView final_view);
  void destroy_targets();
  bool create_swapchain(std::string& error);
  void destroy_swapchain(bool keep_handle);

  bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                     Buffer& out);
  void destroy_buffer(Buffer& b);
  bool create_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                    VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect, Image& out);
  void destroy_image(Image& i);
  bool upload_buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
  bool submit_once(const std::function<void(VkCommandBuffer)>& record);
  // Frames submitted, and how many of them the GPU is known to be done
  // with (an object the frames used can go once done >= its tag).
  uint64_t frames_submitted() const { return frames_submitted_; }
  uint64_t frames_done() const { return frames_done_; }
  void record_scene(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view,
                    VkImageLayout final_layout, const FrameConstants& constants);
  // Copies the changed overlay layers into their buffer (none of the GPU's
  // frames may be using it); false when none is staged.
  bool stage_overlay();

  Options options_;
  std::string device_name_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  float anisotropy_ = 1.0f;
  uint64_t frames_submitted_ = 0, frames_done_ = 0;
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
  VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;
  VkFormat color_format_ = VK_FORMAT_UNDEFINED;

  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;

  Buffer vertex_buffer_, index_buffer_;
  uint32_t index_count_ = 0;
  Image texture_;

  uint32_t target_width_ = 0, target_height_ = 0;
  // render_timed: the kept target and the timestamp queries.
  Image bench_output_;
  VkQueryPool timestamps_ = VK_NULL_HANDLE;
  double timestamp_ns_ = 0;  // ns per tick; 0 without timestamps
  static constexpr uint32_t kTimestamps = 16;
  bool timing_ = false;
  uint32_t mark_end_ = 1;
  std::vector<const char*> mark_labels_;
  std::vector<std::pair<std::string, double>> stage_ms_;
  bool depth_readable_ = false;
  Image msaa_color_, depth_;
  // The post chain: the resolved HDR scene and the quarter-size bloom pair.
  Image hdr_, bloom_a_, bloom_b_;
  uint32_t bloom_width_ = 0, bloom_height_ = 0;
  VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
  VkPipeline post_quarter_ = VK_NULL_HANDLE, post_final_ = VK_NULL_HANDLE;
  VkDescriptorPool post_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet post_sets_[4] = {};  // bright (hdr), blur x (a), blur y (b), composite (hdr, a)
  VkSampler post_sampler_ = VK_NULL_HANDLE;
  // The sun shadow map (D16).
  Image shadow_map_;
  VkSampler shadow_sampler_ = VK_NULL_HANDLE;
  uint32_t shadow_size_ = 0;
  static constexpr VkFormat kShadowFormat = VK_FORMAT_D16_UNORM;

  // Windowed.
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D swapchain_extent_{};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkImageView> swapchain_views_;
  std::vector<VkSemaphore> render_done_;  // one per swapchain image
  VkSemaphore image_ready_ = VK_NULL_HANDLE;
  VkFence frame_fence_ = VK_NULL_HANDLE;
  VkCommandBuffer frame_cmd_ = VK_NULL_HANDLE;
  bool swapchain_dirty_ = false;

  SceneHooks scene_;
  struct OverlayLayer {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    int32_t x = 0, y = 0;
    bool dirty = false;           // set since last copied into the buffer
    bool staged = false;          // this frame copies it
    VkOffset2D at{};
    VkDeviceSize offset = 0;      // in overlay_buffer_
  };
  OverlayLayer overlays_[kOverlayLayers];
  bool overlay_staged_ = false;     // this frame copies a layer
  bool overlay_supported_ = true;   // the swapchain allows transfers into it
  Buffer overlay_buffer_;
  // The UI layer (made on first use).
  bool create_ui(std::string& error);
  void record_ui(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view, VkImageLayout& layout,
                 VkPipelineStageFlags2& last_stage, VkAccessFlags2& last_access);
  bool ui_ready_ = false, ui_failed_ = false;
  VkPipeline ui_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout ui_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout ui_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet ui_set_ = VK_NULL_HANDLE;
  VkSampler ui_sampler_ = VK_NULL_HANDLE;
  VkFormat ui_format_ = VK_FORMAT_UNDEFINED;
  Buffer ui_font_, ui_instances_, ui_staging_;
  void* ui_instances_mapped_ = nullptr;
  void* ui_staging_mapped_ = nullptr;
  std::vector<ui::Instance> ui_items_;
  struct UiAtlasPut {
    uint32_t x, y, w, h;
    std::vector<uint8_t> rgba;
  };
  std::vector<UiAtlasPut> ui_puts_;
  Image ui_atlas_;
  void* overlay_mapped_ = nullptr;
  uint32_t validation_messages_ = 0;
  friend class Showroom;
  friend VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                       VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT*,
                                                       void*);
};

}  // namespace carview
