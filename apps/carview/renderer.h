// A minimal Vulkan 1.3 renderer for carview: dynamic rendering, one
// pipeline, a depth buffer, MSAA and one texture. It renders either to an
// SDL window's swapchain or offscreen (headless, no window system at all).
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <span>
#include <string>
#include <vector>

#include <volk.h>

VK_DEFINE_HANDLE(VmaAllocator);
VK_DEFINE_HANDLE(VmaAllocation);

#include "render_modern.h"
#include "rhi/rhi_types.h"
#include "frame_inspector.h"
#include "upscaler.h"
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
class WorldGpu;
class RtScene;
class RtPipeline;

// Lets another scene (the showroom) record its own draws in place of the
// single car pipeline: `before` runs outside rendering (buffer updates),
// `draw` inside it with the viewport and scissor set.
struct SceneHooks {
  std::function<void(VkCommandBuffer)> before;
  // Optional depth-only prepass before the main colour pass (reduces fragment overdraw).
  std::function<void(VkCommandBuffer)> draw_z_prepass;
  std::function<void(VkCommandBuffer)> draw;
  // Optional predicate: when set and returning false, skips the second MSAA
  // particle pass so color/depth resolve in a single pass with DONT_CARE stores.
  std::function<bool()> has_particles;
  // Optional second subpass after opaque depth has been resolved into
  // `depth_resolved_view()` (for soft depth-faded particles & glows).
  std::function<void(VkCommandBuffer)> draw_particles;
  // Optional Hi-Z pyramid generation from resolved depth (`depth_resolved_view()`).
  std::function<void(VkCommandBuffer, VkImageView, uint32_t, uint32_t, bool)> build_hiz;
  // Optional hardware Ray Tracing / Path Tracing pass running on HDR color + resolved depth + motion vectors
  // right before record_post(). `hdr_img` enters and leaves in VK_IMAGE_LAYOUT_GENERAL.
  std::function<void(VkCommandBuffer, VkImage hdr_img, VkImageView hdr_view, VkImageView depth_view,
                     VkImageView motion_view, uint32_t width, uint32_t height)>
      record_rt;
  float clear[4] = {0.16f, 0.18f, 0.21f, 1.0f};
};

// The drive's post chain (SHOWROOM_LIGHTING, Post chain): the scene is
// drawn as linear HDR (RGBA16F), then a composite tone-maps it into the
// output. The defaults are Colorado's TrackSettings.xml (filmicTone,
// DesaturateAmount, DesatLuminosity, bloom cutoff and scale).
struct PostSettings {
  bool enabled = false;
  bool bloom = true;                                                        // 5-level multi-mip bloom pyramid
  bool auto_exposure = false;                                               // 4-key adaptive exposure (SKY_AND_TOD §5.2)
  bool lut_enabled = false;                                                 // 16^3 3D colour-grading LUT
  float hable[6] = {0.700001f, 0.038f, 0.634f, 0.220001f, 0.003f, 0.107f};  // A, B, C, D, E, F
  float white = 1.0f;
  float exposure = 0.31f;   // linear multiplier (fixed) or filmic_exposure+bias EV (auto_exposure)
  float desaturate = 0.9f;  // the colour's share; the rest is its luminance
  float luminance[3] = {0.2126f, 0.7152f, 0.0722f};
  float bloom_cutoff = 1.0f, bloom_scale = 1.0f;
  float bloom_lower_grad = 1.0f, bloom_upper_grad = 1.0f;
  float bloom_weights[5] = {0.30f, 0.40f, 0.48f, 0.60f, 0.66f};
  float balance[3] = {1.0f, 1.0f, 1.0f};
  float exp_keys[4] = {0.0f, 4.0f, 13.0f, 65.0f};
  float exp_ev[4] = {1.300f, 0.437f, 0.001f, -0.800f};
  float adapt_alpha = 1.0f;     // temporal adaptation factor per frame (1.0 = instant snap)
  float vignette_amount = 0.0f; // 0..1 elliptical vignette strength
  // Optical post-FX (SPEC-VFX-AND-PARTICLES, PostProcessingZones_Safe.xml, dynamicpost.zip):
  bool motion_blur = false;
  bool dof = false;
  bool god_rays = false;
  bool lens_flare = false;
  bool paired_vignette = false;
  float motion_blur_strength = 0.0f;     // 0..1 radial blur intensity
  float motion_blur_center[2] = {0.5f, 0.45f};
  float motion_blur_car_protect = 6.5f;  // metres: keeps player car crisp
  float dof_focal_distance = 12.0f;      // metres
  float dof_focal_range = 38.0f;         // metres
  float dof_max_blur = 0.0f;             // 0..1 bokeh blend
  float near_plane = 0.25f;
  float sun_screen_uv[2] = {0.5f, 0.2f};
  float sun_visibility = 0.0f;           // 0..1 when sun is in front & above horizon
  float sun_view_dir[3] = {0.0f, 0.7f, -0.7f};  // unit sun direction in camera view space (for SSCS)
  float sun_elev_vis = 0.0f;             // 0..1 sun elevation visibility (valid at all camera angles)
  float god_ray_strength = 0.48f;
  float sun_scatter_color[3] = {1.0f, 0.80f, 0.50f};
  float lens_flare_strength = 0.36f;
  float contrast = 1.0f;
  float brightness = 0.0f;
  float sepia = 0.0f;            // 0..1 Photo Mode sepia tone blend
  float vig_top_color[3] = {0.0f, 0.0f, 0.0f};
  float vig_top_opacity = 0.0f;
  float vig_top_center[2] = {0.5f, 0.0f};
  float vig_top_radius[2] = {0.8f, 0.5f};
  float vig_top_rot = 0.0f;
  float vig_top_feather = 0.4f;
  float vig_bot_color[3] = {0.0f, 0.0f, 0.0f};
  float vig_bot_opacity = 0.0f;
  float vig_bot_center[2] = {0.5f, 1.0f};
  float vig_bot_radius[2] = {0.8f, 0.5f};
  float vig_bot_rot = 0.0f;
  float vig_bot_feather = 0.4f;
  float time_sec = 0.0f;
};

// The drive's sun shadow (ours): one orthographic cascade around the car,
// drawn from the sun each frame. size 0: none (a 1x1 map stays bound).
struct ShadowSettings {
  uint32_t size = 0;      // the map's side in texels per cascade
  float radius = 60.0f;   // half the near/base box's side, metres
  uint32_t taps = 4;      // 1: one filtered compare; 4: four; 12/16 via PCSS
  float strength = 0.35f; // the sun's share left in shadow (TrackSettings ShadowColor, read as ours)
  uint32_t cascades = 2;  // 2 (low/fast PCF) or 3 (Near ~20m / Mid ~90m / Far ~320m stabilized D32 CSM)
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
    // Frames the CPU may record ahead of the GPU (1..kFramesInFlight): 2 hides the CPU/GPU
    // sync stall; 1 is the lock-step of the earlier renderer.
    uint32_t frames_in_flight = 2;
    modern::VSync vsync = modern::VSync::kFifo;
    bool backface_cull = true;
    bool z_prepass = false;
    modern::GraphicsSettings graphics;
    // Optional on-disk VkPipelineCache path (e.g. <play>/cache/vk_pipeline_cache.bin).
    std::filesystem::path pipeline_cache_path;
    // UI atlas side in pixels (default 4096; 2048 on low-memory presets saves 48 MiB VRAM).
    uint32_t ui_atlas_size = kUiAtlas;
    // Requested RHI graphics backend (Auto, Vulkan, Metal, D3D12).
    shift::rhi::BackendKind requested_backend = shift::rhi::BackendKind::Auto;
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
  // Frames in flight (Slice 11): the CPU records frame N+1 while the GPU
  // still draws frame N. Per-frame state (command buffer, fence, acquire
  // semaphore, staging and instance buffers, timestamp pool) lives in one of
  // kFramesInFlight slots; a dynamic buffer the CPU writes every frame is
  // kFramesInFlight copies, indexed by frame_slot().
  static constexpr uint32_t kFramesInFlight = 3;  // the most slots; frames_in_flight() of them are used
  uint32_t frames_in_flight() const { return frames_in_flight_; }
  // The device offers what the GPU-driven world needs (see create_device).
  bool gpu_driven_supported() const { return gpu_driven_; }
  VkPipelineCache pipeline_cache() const { return pipeline_cache_; }
  bool pipeline_cache_loaded() const { return pipeline_cache_loaded_; }
  size_t pipeline_cache_bytes() const { return pipeline_cache_loaded_bytes_; }
  void save_pipeline_cache() const;
  // Pre-allocates offscreen/scene render targets ahead of Frame 0 so the first
  // render_timed() / render_offscreen() call incurs zero target-creation hitch.
  bool prewarm_targets(uint32_t width, uint32_t height, std::string& error);
  // V-sync (live): FIFO, MAILBOX or IMMEDIATE; takes effect on the next frame
  // (the swapchain is rebuilt) and falls back to what the surface offers.
  void set_vsync(modern::VSync v) {
    if (v != vsync_) vsync_ = v, swapchain_dirty_ = true;
  }
  // Focus state (alt-tab fast path): never triggers wait_idle or swapchain
  // rebuild; only trims vkAcquireNextImageKHR timeout while unfocused so the
  // event loop stays instantly responsive on macOS MoltenVK and Windows.
  void set_window_focused(bool focused) { window_focused_ = focused; }
  bool window_focused() const { return window_focused_; }
  // Marks the swapchain dirty for clean rebuild on the next draw (e.g. Android foreground resume).
  void invalidate_swapchain() { swapchain_dirty_ = true; }
  uint32_t swapchain_recreate_count() const { return swapchain_recreate_count_; }
  uint32_t target_recreate_count() const { return target_recreate_count_; }
  static bool needs_swapchain_recreate(bool swapchain_dirty, bool has_swapchain, uint32_t cur_w, uint32_t cur_h,
                                       uint32_t ext_w, uint32_t ext_h) {
    return swapchain_dirty || !has_swapchain || cur_w != ext_w || cur_h != ext_h;
  }
  static bool needs_target_recreate(bool targets_dirty, bool has_targets, uint32_t cur_w, uint32_t cur_h,
                                    uint32_t target_w, uint32_t target_h, bool format_changed) {
    return targets_dirty || !has_targets || cur_w != target_w || cur_h != target_h || format_changed;
  }
  static bool should_mark_dirty_on_present(VkResult result, uint32_t cur_w, uint32_t cur_h, uint32_t ext_w,
                                           uint32_t ext_h) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return true;
    if (result == VK_SUBOPTIMAL_KHR) return cur_w != ext_w || cur_h != ext_h;
    return false;
  }
  modern::VSync vsync_requested() const { return vsync_; }
  modern::VSync vsync_active() const { return vsync_active_; }
  uint32_t frame_slot() const { return slot_; }
  // Blocks until every submitted frame is done (a resize, a device-wide
  // change); every object retired so far may then go.
  void wait_idle();
  // GPU time of the last finished windowed frame (two frames behind) and its
  // marked stages, for the F3 overlay and the frame inspector; ms < 0 when
  // the queue has no timestamps.
  double gpu_frame_ms() const { return gpu_frame_ms_; }
  const std::vector<std::pair<std::string, double>>& gpu_stages() const { return slot_stages_; }
  // Records a GPU timestamp pair per marked stage in windowed frames.
  void set_gpu_timing(bool on) { gpu_timing_ = on; }
  bool gpu_timing() const { return gpu_timing_; }
  // The format the scene's pipelines draw into: the output's, or RGBA16F
  // under the post chain.
  VkFormat scene_format() const { return options_.post.enabled ? VK_FORMAT_R16G16B16A16_SFLOAT : color_format_; }
  bool post() const { return options_.post.enabled; }
  bool reverse_z() const { return options_.reverse_z; }
  bool backface_cull() const { return options_.backface_cull; }
  bool z_prepass() const { return options_.z_prepass; }
  void set_z_prepass(bool on) { options_.z_prepass = on; }
  // The MSAA sample count in use (1 when off).
  uint32_t sample_count() const { return uint32_t(samples_); }
  // The anisotropy the scene's samplers use: options.anisotropy within the
  // device's limit, 1 when the device has none.
  float sampler_anisotropy() const { return anisotropy_; }
  // Windowed: renders and presents one frame. Returns false on a fatal error.
  bool draw_frame(const FrameConstants& constants, std::string& error);
  // Real swapchain / color-target PNG capture (F8, F12, Photo Mode): captures
  // the next presented frame into RGBA8 (either with UI/overlays or clean
  // scene-only before UI). Call take_captured_frame() after draw_frame().
  void request_capture(bool include_ui = true);
  bool capture_pending() const { return capture_requested_; }
  bool take_captured_frame(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

  // With hooks set, frames record the hooked scene; upload() is not needed.
  void set_scene(SceneHooks hooks) { scene_ = std::move(hooks); }
  // Updates per-frame post-processing parameters (TOD curve, auto-exposure, bloom, vignette).
  void set_post_settings(const PostSettings& post) {
    const bool was_enabled = options_.post.enabled;
    const bool bloom_changed = post.bloom != options_.post.bloom;
    const bool mblur_changed = post.motion_blur != options_.post.motion_blur;
    options_.post = post;
    options_.post.enabled = was_enabled;
    if (was_enabled && (bloom_changed || (mblur_changed && graphics_.velocity_blur != modern::VelocityBlur::kOff))) {
      targets_dirty_ = true;
    }
  }
  const PostSettings& post_settings() const { return options_.post; }
  // Queues an updated N^3 RGBA8 3D colour-grading LUT (default 16^3, supports 16/17/33) for upload before post.
  void set_post_lut(std::span<const uint8_t> rgba, uint32_t size = 16);
  void set_lut_strength(float pct) { lut_strength_ = std::clamp(pct, 0.0f, 100.0f) / 100.0f; }
  void reset_auto_exposure() { lum_reset_ = true; }

  // Modern graphics pipeline controls (render scale, SSAO, FXAA, VRS, HDR10/scRGB, HUD layout).
  void set_graphics_settings(const modern::GraphicsSettings& g);
  const modern::GraphicsSettings& graphics_settings() const { return graphics_; }

  // Multi-backend RHI startup diagnostic info (for F3 overlay & logs).
  shift::rhi::BackendKind requested_backend() const { return requested_backend_; }
  shift::rhi::BackendKind probed_rhi_backend() const { return probed_rhi_backend_; }
  const std::string& active_backend_label() const { return active_backend_label_; }
  const std::string& probed_rhi_device_name() const { return probed_rhi_device_name_; }
  void set_camera_projection(float near_plane, float far_plane, float proj00, float proj11);
  void set_camera_vp(const float vp[16], float jitter_uv_x = 0.0f, float jitter_uv_y = 0.0f);
  // Phase B Volumetric Sun Shafts / Aerial Perspective / Night Headlight Cones UBO state.
  struct VolumetricBeam {
    float origin_range[4] = {0, 0, 0, 1};
    float dir_power[4] = {0, 0, 1, 0};
    float color_cos[4] = {1, 1, 1, 0.7f};
  };
  struct VolumetricState {
    float inv_view_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float shadow_matrix[16] = {};
    float shadow_matrix_mid[16] = {};
    float shadow_matrix_far[16] = {};
    float eye_time[4] = {0, 0, 0, 0};
    float sun_dir_vis[4] = {0, 1, 0, 0};
    float sun_col_str[4] = {1, 0.85f, 0.6f, 0.45f};
    float fog_col_dens[4] = {0.1f, 0.14f, 0.16f, 0.001f};
    float shadow_p[4] = {0, 0, 0, 0};  // shadows_on, three_cascades, night_factor, beam_count
    float up_eyey[4] = {0, 1, 0, 150.0f};
    float height_p[4] = {200.0f, 4000.0f, 0.9f, 0.0f};
    VolumetricBeam beams[8];
  };
  static_assert(sizeof(VolumetricState) == 256 + 7 * 16 + 8 * 48);
  void set_volumetric_state(const VolumetricState& state) {
    vol_state_ = state;
    vol_state_valid_ = true;
  }
  void set_hud_layout(const modern::HudLayout& layout) { hud_layout_ = layout; }
  void set_hdr_pattern(uint32_t pattern, float pattern_nits = 1000.0f) {
    if ((pattern != 0) != (hdr_pattern_ != 0)) targets_dirty_ = true;
    hdr_pattern_ = pattern;
    hdr_pattern_nits_ = pattern_nits;
  }
  modern::ModernTargetPlan allocated_modern_targets() const { return allocated_targets_; }
  void set_inspector_preview(bool active, InspectorView view);
  void set_hiz_preview_view(VkImageView view);
  bool take_inspector_preview(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
  static constexpr uint32_t kInspectorPreviewW = 320;
  static constexpr uint32_t kInspectorPreviewH = 180;
  const UpscalerRuntimeInfo& upscaler_runtime() const { return upscaler_runtime_; }
  modern::UpscalerChoice active_upscaler() const {
    return modern::resolve_upscaler(graphics_.upscaler, upscaler_runtime_.support,
                                    {render_width(), render_height()}, {target_width(), target_height()});
  }
  modern::FrameGen active_frame_gen() const {
    return modern::resolve_frame_gen(graphics_.frame_gen, upscaler_runtime_.support);
  }
  bool vrs_supported() const { return vrs_supported_; }
  bool vrs_active() const { return vrs_supported_ && graphics_.vrs; }
  void apply_vrs(VkCommandBuffer cmd) const;
  bool hdr10_supported() const { return hdr10_supported_; }
  bool scrgb_supported() const { return scrgb_supported_; }
  modern::HdrMode hdr_active() const { return hdr_mode_; }

  // Hardware Ray Tracing & Path Tracing capabilities (RT-1..RT-4).
  bool rt_supported() const { return rt_caps_.supports_hybrid(); }
  const rt::RtDeviceCaps& rt_caps() const { return rt_caps_; }
  rt::RtDecision rt_decision() const { return rt::resolve_rt_mode(graphics_.rt_mode, rt_caps_); }
  rt::RtMode active_rt_mode() const { return rt_decision().active_mode; }
  VkDeviceAddress buffer_device_address(VkBuffer buffer) const;
  void set_photo_pt_accumulate(bool active, bool reset = false) {
    if (active != photo_pt_accum_ || reset) photo_pt_reset_ = true;
    photo_pt_accum_ = active;
  }
  bool photo_pt_accumulate() const { return photo_pt_accum_; }
  bool consume_photo_pt_reset() {
    const bool r = photo_pt_reset_;
    photo_pt_reset_ = false;
    return r;
  }

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
  static constexpr uint32_t kUiAtlas = 4096;
  void ui_atlas_put(uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::span<const uint8_t> rgba);

  const std::string& device_name() const { return device_name_; }
  struct DeviceInfo {
    std::string device_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    std::string device_type = "unknown";
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    std::string driver_name;
    std::string driver_info;
    bool portability_subset = false;
    bool is_moltenvk = false;
    bool is_turnip = false;
    bool has_timestamps = false;
  };
  const DeviceInfo& device_info() const { return device_info_; }
  double pipeline_compile_ms() const { return pipeline_compile_ms_; }
  double last_submit_wait_ms() const { return last_submit_wait_ms_; }
  uint32_t last_ui_shape_count() const { return uint32_t(ui_items_.size()); }
  uint64_t staging_upload_bytes() const { return staging_upload_bytes_; }
  uint32_t target_width() const { return target_width_; }
  uint32_t target_height() const { return target_height_; }
  uint32_t render_width() const { return render_width_ ? render_width_ : target_width_; }
  uint32_t render_height() const { return render_height_ ? render_height_ : target_height_; }
  modern::Extent expected_render_extent(uint32_t out_w, uint32_t out_h) const {
    return options_.post.enabled ? modern::scene_extent({out_w, out_h}, graphics_.scale, graphics_.base_540p)
                                 : modern::Extent{out_w, out_h};
  }
  VkImageView depth_resolved_view() const { return depth_resolved_.view; }
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
  bool create_modern_passes(std::string& error);
  void destroy_modern_passes();
  void update_modern_descriptors();
  bool ensure_lut_image(uint32_t size, std::string& error);
  // The shadow map (options_.shadow.size, or 1x1 without shadows) and its
  // comparison sampler, made once; left in DEPTH_STENCIL_READ_ONLY_OPTIMAL.
  bool ensure_shadow_map(std::string& error);
  void record_post(VkCommandBuffer cmd, VkImageView final_view, VkPipeline pipe, float out_alpha);
  void record_ssao(VkCommandBuffer cmd);
  void record_sscs(VkCommandBuffer cmd);
  void record_ssr(VkCommandBuffer cmd);
  void record_volumetric(VkCommandBuffer cmd);
  void record_fxaa(VkCommandBuffer cmd, VkDescriptorSet set, VkImage dst_img, VkImageView dst_view, uint32_t w,
                   uint32_t h);
  void record_upscale(VkCommandBuffer cmd, VkImage src_img, VkDescriptorSet src_set, bool src_is_fxaa,
                      uint32_t src_w, uint32_t src_h, VkImage dst_img, VkImageView dst_view, uint32_t dst_w,
                      uint32_t dst_h);
  void record_motion_vectors(VkCommandBuffer cmd);
  void record_mblur_tiles(VkCommandBuffer cmd);
  void record_inspector_preview(VkCommandBuffer cmd, VkBuffer readback_dst);
  void record_hdr_output(VkCommandBuffer cmd, VkDescriptorSet src_set, VkImage dst_img, VkImageView dst_view,
                         VkFormat dst_format, uint32_t w, uint32_t h);
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
                    VkImageLayout final_layout, const FrameConstants& constants,
                    VkBuffer capture_dst = VK_NULL_HANDLE, bool capture_before_ui = false,
                    VkFormat final_format = VK_FORMAT_UNDEFINED, VkBuffer inspector_dst = VK_NULL_HANDLE);
  // True when record_scene routes post -> stand_in_ (VK_FORMAT_R8G8B8A8_UNORM)
  // before blitting/tonemapping to final_image.
  bool uses_modern_chain() const;
  // Copies the changed overlay layers into their buffer (none of the GPU's
  // frames may be using it); false when none is staged. `final_format` is the
  // non-modern-chain target format (ignored when uses_modern_chain() is true,
  // since overlays copy into R8G8B8A8_UNORM stand_in_).
  bool stage_overlay(VkFormat final_format = VK_FORMAT_UNDEFINED);

  Options options_;
  shift::rhi::BackendKind requested_backend_ = shift::rhi::BackendKind::Auto;
  shift::rhi::BackendKind probed_rhi_backend_ = shift::rhi::BackendKind::Vulkan;
  std::string active_backend_label_ = "Vulkan 1.3";
  std::string probed_rhi_device_name_;
  std::string device_name_;
  uint32_t device_vendor_id_ = 0;
  DeviceInfo device_info_;
  double pipeline_compile_ms_ = 0.0;
  double last_submit_wait_ms_ = 0.0;
  uint64_t staging_upload_bytes_ = 0;
  UpscalerRuntimeInfo upscaler_runtime_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  modern::PipelineCacheKey pipeline_cache_key_{};
  mutable bool pipeline_cache_loaded_ = false;
  mutable size_t pipeline_cache_loaded_bytes_ = 0;
  bool lazy_memory_supported_ = false;
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
  uint32_t render_width_ = 0, render_height_ = 0;
  modern::GraphicsSettings graphics_;
  modern::HudLayout hud_layout_;
  float cam_near_ = 0.25f, cam_far_ = 4000.0f, cam_proj00_ = 1.0f, cam_proj11_ = -1.0f;
  float cam_vp_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float prev_cam_vp_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float cam_jitter_uv_x_ = 0.0f, cam_jitter_uv_y_ = 0.0f;
  float prev_jitter_uv_x_ = 0.0f, prev_jitter_uv_y_ = 0.0f;
  bool cam_vp_set_ = false, prev_cam_vp_valid_ = false;
  bool inspector_active_ = false;
  InspectorView inspector_view_ = InspectorView::kColor;
  VkImageView hiz_preview_view_ = VK_NULL_HANDLE;
  bool inspector_has_rgba_ = false;
  std::vector<uint8_t> inspector_rgba_;
  Buffer inspector_offscreen_buf_;
  float lut_strength_ = 1.0f;
  uint32_t lut_image_size_ = 16, lut_pending_size_ = 16;
  uint32_t hdr_pattern_ = 0;
  float hdr_pattern_nits_ = 1000.0f;
  bool vrs_supported_ = false;
  rt::RtDeviceCaps rt_caps_{};
  bool photo_pt_accum_ = false;
  bool photo_pt_reset_ = true;
  bool hdr10_supported_ = false, scrgb_supported_ = false, hdr_colorspace_ext_ = false;
  modern::HdrMode hdr_mode_ = modern::HdrMode::kOff;
  VkColorSpaceKHR swapchain_colorspace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  bool targets_dirty_ = false;
  VkFormat target_color_format_ = VK_FORMAT_UNDEFINED;
  bool window_focused_ = true;
  uint32_t swapchain_recreate_count_ = 0;
  uint32_t target_recreate_count_ = 0;
  // render_timed: the kept target and the timestamp queries.
  Image bench_output_;
  VkQueryPool timestamps_ = VK_NULL_HANDLE;
  double timestamp_ns_ = 0;  // ns per tick; 0 without timestamps
  static constexpr uint32_t kTimestamps = 64;
  bool timing_ = false;
  uint32_t mark_end_ = 1;
  std::vector<const char*> mark_labels_;
  std::vector<std::pair<std::string, double>> stage_ms_;
  bool depth_readable_ = false;
  bool depth_resolve_supported_ = false;
  Image msaa_color_, depth_, depth_resolved_;
  // The post chain: resolved HDR scene, 6-level Jimenez/Dual-Kawase bloom pyramid, auto-exposure, N^3 3D LUT.
  static constexpr uint32_t kBloomLevels = 6;
  static constexpr uint32_t kPostSetCount = 14;
  Image hdr_;
  Image bloom_a_[kBloomLevels], bloom_b_[kBloomLevels];
  uint32_t bloom_w_[kBloomLevels] = {}, bloom_h_[kBloomLevels] = {};
  Image lum_16_, lum_adapt_[2];
  Image lut3d_;
  std::vector<uint8_t> lut_pending_;
  bool lut_dirty_ = false;
  bool lum_reset_ = true;
  VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
  VkPipeline post_quarter_ = VK_NULL_HANDLE, post_bloom_down_ = VK_NULL_HANDLE, post_bloom_up_ = VK_NULL_HANDLE,
             post_final_ = VK_NULL_HANDLE, post_final_sdr_ = VK_NULL_HANDLE;
  VkDescriptorPool post_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet post_sets_[kPostSetCount] = {};
  VkSampler post_sampler_ = VK_NULL_HANDLE;
  VkSampler post_depth_sampler_ = VK_NULL_HANDLE;
  Buffer post_optical_buf_;  // device-local, filled by vkCmdUpdateBuffer in the frame's commands
  // Modern post-FX & HDR targets (XeGTAO + Bent Normals, SSCS, Hi-Z SSR, Volumetric Scattering, McGuire motion blur, FXAA, upscale, HDR10/scRGB).
  modern::ModernTargetPlan allocated_targets_{};
  Image ao_white_, vel_zero_, ssr_zero_, vol_identity_, ao_raw_, ao_tmp_, ao_blur_, sscs_;
  Image ssr_raw_, ssr_blur_, vol_raw_, vol_blur_;
  uint32_t ao_width_ = 0, ao_height_ = 0;
  uint32_t sscs_width_ = 0, sscs_height_ = 0;
  uint32_t ssr_width_ = 0, ssr_height_ = 0;
  uint32_t vol_width_ = 0, vol_height_ = 0;
  Buffer vol_ubo_;
  VolumetricState vol_state_{};
  bool vol_state_valid_ = false;
  Image scene_sdr_, scene_fxaa_, upscale_tmp_, stand_in_, stand_in_fxaa_;
  Image motion_vec_, mblur_tile_, mblur_neighbor_, temporal_hist_[2], inspector_thumb_;
  uint32_t mblur_tile_w_ = 0, mblur_tile_h_ = 0;
  uint32_t temporal_idx_ = 0;
  bool temporal_reset_ = true;
  VkDescriptorSetLayout ssao_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout single_tex_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout temporal_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout ssr_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout vol_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool modern_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout ssao_layout_ = VK_NULL_HANDLE, sscs_layout_ = VK_NULL_HANDLE,
                   ssr_layout_ = VK_NULL_HANDLE, vol_layout_ = VK_NULL_HANDLE,
                   mblur_tile_layout_ = VK_NULL_HANDLE, fxaa_layout_ = VK_NULL_HANDLE,
                   upscale_layout_ = VK_NULL_HANDLE, hdr_layout_ = VK_NULL_HANDLE,
                   motion_layout_ = VK_NULL_HANDLE, temporal_layout_ = VK_NULL_HANDLE,
                   inspector_layout_ = VK_NULL_HANDLE;
  VkPipeline ssao_pipe_ = VK_NULL_HANDLE, sscs_pipe_ = VK_NULL_HANDLE, ssr_pipe_ = VK_NULL_HANDLE,
             vol_pipe_ = VK_NULL_HANDLE, mblur_tile_pipe_ = VK_NULL_HANDLE,
             fxaa_pipe_ = VK_NULL_HANDLE, upscale_pipe_ = VK_NULL_HANDLE,
             motion_pipe_ = VK_NULL_HANDLE, temporal_pipe_ = VK_NULL_HANDLE, inspector_pipe_ = VK_NULL_HANDLE;
  VkPipeline hdr_pipe_sdr_ = VK_NULL_HANDLE, hdr_pipe_swap_ = VK_NULL_HANDLE;
  VkFormat hdr_swap_fmt_ = VK_FORMAT_UNDEFINED;
  VkDescriptorSet ssao_sets_[3] = {};
  VkDescriptorSet ssr_sets_[2] = {};
  VkDescriptorSet vol_sets_[2] = {};
  VkDescriptorSet sscs_set_ = VK_NULL_HANDLE, mblur_tile_set_ = VK_NULL_HANDLE,
                  mblur_neighbor_set_ = VK_NULL_HANDLE;
  VkDescriptorSet fxaa_set_scene_ = VK_NULL_HANDLE, fxaa_set_standin_ = VK_NULL_HANDLE;
  VkDescriptorSet upscale_set_scene_ = VK_NULL_HANDLE, upscale_set_fxaa_ = VK_NULL_HANDLE,
                  upscale_set_tmp_ = VK_NULL_HANDLE, upscale_set_hist_[2] = {};
  VkDescriptorSet hdr_set_standin_ = VK_NULL_HANDLE, hdr_set_fxaa_ = VK_NULL_HANDLE;
  VkDescriptorSet motion_set_ = VK_NULL_HANDLE;
  VkDescriptorSet temporal_sets_[4] = {};
  VkDescriptorSet inspector_sets_[6] = {};
  bool modern_ready_ = false;
  // The sun shadow map (D32_SFLOAT 2x1 or 3x1 stabilized CSM atlas + raw depth sampler for PCSS/volumetrics).
  Image shadow_map_;
  VkSampler shadow_sampler_ = VK_NULL_HANDLE;
  VkSampler shadow_raw_sampler_ = VK_NULL_HANDLE;
  uint32_t shadow_size_ = 0;
  uint32_t shadow_cascades_ = 2;
  static constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

  // Windowed.
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D swapchain_extent_{};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkImageView> swapchain_views_;
  std::vector<VkSemaphore> render_done_;  // one per swapchain image
  // Frame slots (windowed): see kFramesInFlight.
  struct FrameSlot {
    VkSemaphore image_ready = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t submit_id = 0;  // frames_submitted_ when its frame was submitted (0: none)
    VkQueryPool timestamps = VK_NULL_HANDLE;
    std::vector<const char*> labels;  // the marks of its frame
    uint32_t mark_end = 1;
    bool timed = false;
    Buffer ui_instances, ui_staging, overlay, lut_staging, inspector_readback;
    void* ui_instances_mapped = nullptr;
    void* ui_staging_mapped = nullptr;
    void* overlay_mapped = nullptr;
    void* lut_staging_mapped = nullptr;
    void* inspector_readback_mapped = nullptr;
    bool inspector_ready = false;
    uint64_t overlay_ver[4] = {};        // the version of each overlay layer this slot's buffer holds
    VkDeviceSize overlay_off[4] = {};    // and where
    bool overlay_bgra[4] = {};           // channel order currently staged in this slot's buffer
  };
  FrameSlot slots_[kFramesInFlight];
  uint32_t slot_ = 0;
  uint32_t frames_in_flight_ = 2;  // options.frames_in_flight, SHIFT_FRAMES_IN_FLIGHT (1: the old one-frame lock step)
  bool gpu_driven_ = false;
  modern::VSync vsync_ = modern::VSync::kFifo, vsync_active_ = modern::VSync::kFifo;
  FrameSlot& slot() { return slots_[slot_]; }
  void collect_slot_timing(FrameSlot& s);
  double gpu_frame_ms_ = -1.0;
  std::vector<std::pair<std::string, double>> slot_stages_;
  bool gpu_timing_ = false;
  VkQueryPool ts_pool_ = VK_NULL_HANDLE;  // the pool mark() writes into
  std::mutex queue_mutex_;  // vkQueueSubmit / vkQueuePresent: uploads run on any thread
  std::mutex pool_mutex_;   // the command pool
  std::vector<VkCommandPool> free_pools_, all_pools_;  // for threads other than the render thread's
  VkQueue transfer_queue_ = VK_NULL_HANDLE;  // a second queue of the family for off-thread uploads
  std::thread::id render_thread_;
  bool swapchain_dirty_ = false;

  SceneHooks scene_;
  struct OverlayLayer {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    int32_t x = 0, y = 0;
    bool dirty = false;           // set since last copied into the buffer
    bool staged = false;          // this frame copies it
    VkOffset2D at{};
    VkDeviceSize offset = 0;      // (unused: each slot keeps its own offsets)
    uint64_t version = 1;         // bumped by set_overlay; a slot's buffer holds one version
  };
  OverlayLayer overlays_[kOverlayLayers];
  bool overlay_staged_ = false;     // this frame copies a layer
  bool overlay_supported_ = true;   // the swapchain allows transfers into it
  bool capture_supported_ = false;  // the swapchain allows transfers out of it
  bool capture_requested_ = false;
  bool capture_include_ui_ = true;
  bool capture_ready_ = false;
  uint32_t capture_w_ = 0, capture_h_ = 0;
  std::vector<uint8_t> capture_rgba_;
  Buffer capture_buffer_;
  // The UI layer (made on first use).
  bool create_ui(std::string& error);
  void record_ui(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view, VkImageLayout& layout,
                 VkPipelineStageFlags2& last_stage, VkAccessFlags2& last_access, VkPipeline pipeline,
                 uint32_t width, uint32_t height);
  bool ui_ready_ = false, ui_failed_ = false;
  VkPipeline ui_pipeline_ = VK_NULL_HANDLE, ui_pipeline_sdr_ = VK_NULL_HANDLE;
  VkPipelineLayout ui_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout ui_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet ui_set_ = VK_NULL_HANDLE;
  VkSampler ui_sampler_ = VK_NULL_HANDLE;
  VkFormat ui_format_ = VK_FORMAT_UNDEFINED;
  Buffer ui_font_;
  std::vector<ui::Instance> ui_items_;
  struct UiAtlasPut {
    uint32_t x, y, w, h;
    std::vector<uint8_t> rgba;
  };
  std::vector<UiAtlasPut> ui_puts_;
  Image ui_atlas_;
  uint32_t validation_messages_ = 0;
  friend class Showroom;
  friend class WorldGpu;
  friend class RtScene;
  friend class RtPipeline;
  friend VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                       VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT*,
                                                       void*);
};

}  // namespace carview
