// A minimal Vulkan 1.3 renderer for carview: dynamic rendering, one
// pipeline, a depth buffer, MSAA and one texture. It renders either to an
// SDL window's swapchain or offscreen (headless, no window system at all).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <volk.h>

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

class Renderer {
 public:
  struct Options {
    SDL_Window* window = nullptr;  // null: headless
    bool validation = false;
    uint32_t msaa = 4;
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
  // The MSAA sample count in use (1 when off).
  uint32_t sample_count() const { return uint32_t(samples_); }
  // Windowed: renders and presents one frame. Returns false on a fatal error.
  bool draw_frame(const FrameConstants& constants, std::string& error);

  // With hooks set, frames record the hooked scene; upload() is not needed.
  void set_scene(SceneHooks hooks) { scene_ = std::move(hooks); }

  // An opaque panel drawn on the CPU (RGBA8, already sRGB-encoded) and
  // copied over every following frame with its top-left corner at (x, y)
  // pixels; a negative x or y counts from the right or bottom edge. An
  // empty panel removes it. Frames it does not fit in are drawn without it.
  void set_overlay(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, int32_t x, int32_t y);

  const std::string& device_name() const { return device_name_; }
  uint32_t samples() const { return uint32_t(samples_); }
  uint32_t validation_messages() const { return validation_messages_; }

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool create_instance(std::string& error);
  bool pick_device(std::string& error);
  bool create_device(std::string& error);
  bool create_pipeline(VkFormat color_format, std::string& error);
  bool create_targets(uint32_t width, uint32_t height, std::string& error);
  void destroy_targets();
  bool create_swapchain(std::string& error);
  void destroy_swapchain(bool keep_handle);

  uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const;
  bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                     Buffer& out);
  void destroy_buffer(Buffer& b);
  bool create_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                    VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect, Image& out);
  void destroy_image(Image& i);
  bool upload_buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
  bool submit_once(const std::function<void(VkCommandBuffer)>& record);
  void record_scene(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view,
                    VkImageLayout final_layout, const FrameConstants& constants);
  // Copies the overlay into its buffer (none of the GPU's frames may be
  // using it); false when there is none or it does not fit the target.
  bool stage_overlay();

  Options options_;
  std::string device_name_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
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
  std::vector<uint8_t> overlay_pixels_;
  uint32_t overlay_width_ = 0, overlay_height_ = 0;
  int32_t overlay_x_ = 0, overlay_y_ = 0;
  bool overlay_staged_ = false;     // this frame copies the overlay
  bool overlay_supported_ = true;   // the swapchain allows transfers into it
  VkOffset2D overlay_at_{};
  Buffer overlay_buffer_;
  void* overlay_mapped_ = nullptr;
  uint32_t validation_messages_ = 0;
  friend class Showroom;
  friend VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                       VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT*,
                                                       void*);
};

}  // namespace carview
