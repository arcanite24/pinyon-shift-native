// Pinyon Shift's modern graphics options as plain C++ (Slice 11): the
// internal render scale, the 16:9-locked HUD with its scale and safe zone,
// ultrawide field of view, FXAA / SSAO / VRS / texture-size choices, V-sync
// and frame-rate cap, the HDR10 / scRGB output choice and its encoding
// maths, the output upscaler with its fallbacks, and the .cube colour LUT
// reader. No Vulkan in here, so every rule is unit-tested (tests/
// render_modern_tests.cpp); the renderer applies the result.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carview::modern {

struct Extent {
  uint32_t w = 0, h = 0;
  bool operator==(const Extent& o) const { return w == o.w && h == o.h; }
  bool operator!=(const Extent& o) const { return !(*this == o); }
};

// ---- Internal render scale --------------------------------------------------
// The scene is drawn at 1x = 1280 x 720 (the console's size; 960 x 540 with
// the 540p base) times a whole multiple 1..4, at the window's aspect, and
// scaled to the window by the output upscaler. Scale 0 draws at the window's
// own size ("native").
constexpr int kMinScale = 0, kMaxScale = 4;
int clamp_scale(int scale);
Extent scene_extent(Extent window, int scale, bool base_540p);
// "1x (1280x720)" for the settings page and the F3 panel.
std::string scale_label(int scale, bool base_540p, Extent window);

// ---- Ultrawide ---------------------------------------------------------------
constexpr float kHudAspect = 16.0f / 9.0f;
// The horizontal field of view of a vertical one at an aspect (degrees).
float hfov_from_vfov(float vfov_deg, float aspect);
// The vertical field of view to draw with: the given one (Hor+, the view
// widens on a wider screen), reduced only when the horizontal one would pass
// `max_hfov_deg`.
float effective_vfov(float vfov_deg, float aspect, float max_hfov_deg = 130.0f);

// ---- HUD layout ----------------------------------------------------------------
// The HUD is laid out in a virtual screen (width x height) and placed in the
// window: centred, locked to 16:9 on wider windows, scaled 80..120 % about
// the centre and shrunk by the safe zone (0..10 % of each side).
struct HudLayout {
  float virtual_w = 0, virtual_h = 0;  // what the HUD code lays out in
  float origin_x = 0, origin_y = 0;    // the virtual screen's top left in the window
  float scale = 1;                     // window pixels per virtual pixel
  std::array<float, 2> to_screen(float x, float y) const { return {origin_x + x * scale, origin_y + y * scale}; }
  std::array<float, 2> to_virtual(float x, float y) const {
    return {(x - origin_x) / (scale > 0 ? scale : 1.0f), (y - origin_y) / (scale > 0 ? scale : 1.0f)};
  }
};
HudLayout hud_layout(float window_w, float window_h, bool lock_16_9, float hud_scale_percent, float safe_zone_percent);
constexpr float kMinHudScale = 80.0f, kMaxHudScale = 120.0f, kMaxSafeZone = 10.0f;

// ---- Anti-aliasing, AO, shading, textures ---------------------------------------
enum class AaMode : int { kOff = 0, kFxaaWorld = 1, kFxaaAll = 2 };
enum class SsaoQuality : int { kOff = 0, kLow = 1, kMedium = 2, kHigh = 3 };
// Taps per pixel and the world radius (m) of each level.
int ssao_taps(SsaoQuality q);
float ssao_radius_m(SsaoQuality q);
enum class TextureCap : int { kFull = 0, k1024 = 1024, k512 = 512, k256 = 256 };
// Mip levels to skip so the largest side is at most `cap` (never past the last level).
uint32_t texture_levels_to_skip(uint32_t width, uint32_t height, uint32_t level_count, TextureCap cap);

// ---- Presentation -----------------------------------------------------------------
enum class VSync : int { kFifo = 0, kMailbox = 1, kImmediate = 2 };
// The present mode to use given which the surface offers (bit per VSync); a
// missing one falls back down the list Mailbox -> Fifo (Fifo is always there).
VSync resolve_vsync(VSync requested, uint32_t available_mask);
constexpr uint32_t vsync_bit(VSync v) { return 1u << unsigned(v); }
// The frame-rate cap: 30, 40, 60, 120, 144, 240, or 0 for the display's.
constexpr std::array<int, 6> kFpsCaps = {30, 40, 60, 120, 144, 240};
// Seconds between frames (0: no cap). `display_hz` is used by Display (cap 0).
double fps_cap_interval(int cap, double display_hz);
// Paces a loop to an interval: returns how long to wait now, and keeps the
// schedule across late frames so a hitch does not make the next ones run fast.
class FramePacer {
 public:
  void set_interval(double seconds) { interval_ = seconds; }
  double interval() const { return interval_; }
  // `now` in seconds on a monotonic clock; returns the seconds to sleep (0: go).
  double wait_for(double now);
 private:
  double interval_ = 0, next_ = -1;
};

// ---- HDR --------------------------------------------------------------------------
enum class HdrMode : int { kOff = 0, kHdr10 = 1, kScRgb = 2 };
// The output to use when HDR is wanted: scRGB (extended sRGB linear) first,
// else HDR10 (ST 2084); off when the surface offers neither.
HdrMode resolve_hdr(bool wanted, bool has_hdr10, bool has_scrgb);
// Paper white in nits (the HUD and menus' white), 80..400.
float clamp_paper_white(float nits);
// scRGB: 1.0 is 80 nits.
inline float scrgb_scale(float paper_white_nits) { return clamp_paper_white(paper_white_nits) / 80.0f; }
// ST 2084 (PQ) of an absolute luminance in nits -> 0..1, and back.
float pq_encode(float nits);
float pq_decode(float code);
// BT.709 linear -> BT.2020 linear.
std::array<float, 3> rec709_to_rec2020(const std::array<float, 3>& rgb);
// The highlight expansion the composite applies in HDR: `t` is the tone-mapped
// SDR value in display-linear 0..1 (1 = paper white), `highlights` 0..100 %,
// `peak_nits` the display's peak. Values below 0.5 are untouched.
float expand_highlight(float t, float highlights_percent, float paper_white_nits, float peak_nits);

// ---- Output upscaling ----------------------------------------------------------------
enum class Upscaler : int { kBilinear = 0, kCas = 1, kFsr1 = 2, kFsr2 = 3, kFsr3 = 4, kDlss = 5 };
enum class FrameGen : int { kOff = 0, kFsr3 = 1, kDlss = 2 };
// What this machine can run.
struct UpscalerSupport {
  bool temporal = true;      // motion vectors + jitter path (FSR 2/3 style)
  bool fsr_runtime = false;  // AMD FidelityFX runtime found in upscalers/
  bool dlss_runtime = false; // nvngx_dlss.dll found in upscalers/
  bool rtx = false;          // an RTX class GPU
  bool frame_gen = false;    // the frame-generation hook
};
struct UpscalerChoice {
  Upscaler requested = Upscaler::kBilinear;
  Upscaler active = Upscaler::kBilinear;  // what actually runs
  bool unavailable = false;               // requested but not runnable: "(UNAVAILABLE)", FSR 1 used
  bool native_aa = false;                 // render size equals output size: DLAA / native AA
  bool temporal = false;                  // jitter + motion vectors needed
  std::string label;                      // "FSR 1", "DLSS (UNAVAILABLE)", ...
};
UpscalerChoice resolve_upscaler(Upscaler requested, const UpscalerSupport& support, Extent render, Extent output);
FrameGen resolve_frame_gen(FrameGen requested, const UpscalerSupport& support, bool hdr_ok = true);
const char* upscaler_name(Upscaler u);
const char* frame_gen_name(FrameGen f);
// The camera jitter of frame `index` (Halton 2,3; -0.5..0.5 pixel) for `phase_count` phases.
std::array<float, 2> halton_jitter(uint32_t index, uint32_t phase_count);
// FSR's phase count for a scale: 8 * (output / render)^2, at least 8.
uint32_t jitter_phase_count(Extent render, Extent output);

// ---- Colour LUT (.cube) -----------------------------------------------------------------
struct CubeLut {
  std::string title;
  uint32_t size = 0;                 // N points per side (17 and 33 are the usual)
  std::array<float, 3> domain_min{0, 0, 0}, domain_max{1, 1, 1};
  std::vector<float> rgb;            // N^3 triples, red fastest
  bool valid() const { return size >= 2 && rgb.size() == size_t(size) * size * size * 3; }
};
// Reads an Adobe/Resolve .cube 3D LUT; false with `error` set on a 1D LUT or
// any malformed line. Values are kept as written (the domain is applied when
// the table is converted).
bool parse_cube(std::string_view text, CubeLut& out, std::string& error);
// The table as RGBA8 (red fastest), each channel mapped through the domain and clamped to 0..1.
std::vector<uint8_t> cube_to_rgba8(const CubeLut& lut);
// Sampling the table the way the shader does (trilinear, texel-centre addressing), for tests.
std::array<float, 3> sample_cube(const CubeLut& lut, const std::array<float, 3>& rgb);

// ---- The settings as one value ---------------------------------------------------------------
struct GraphicsSettings {
  int scale = 1;               // 0 native, 1..4
  bool base_540p = false;
  AaMode aa = AaMode::kOff;
  SsaoQuality ssao = SsaoQuality::kOff;
  std::string lut;             // file name in the luts folder, empty: none
  float lut_strength = 100.0f; // percent
  bool vrs = false;
  TextureCap texture_cap = TextureCap::kFull;
  VSync vsync = VSync::kFifo;
  int fps_cap = 60;            // 0: the display's refresh rate
  Upscaler upscaler = Upscaler::kBilinear;
  float sharpening = 50.0f;    // percent (CAS / FSR RCAS)
  FrameGen frame_gen = FrameGen::kOff;
  bool hdr = false;
  bool auto_hdr = false;       // Windows: switch the display to HDR for the run
  float paper_white = 203.0f;  // nits
  float hdr_highlights = 50.0f;// percent
  float peak_nits = 1000.0f;
  bool hdr_compare = false;
  float hud_scale = 100.0f;
  float safe_zone = 0.0f;
  bool hud_lock_16_9 = true;
  bool operator==(const GraphicsSettings&) const = default;
};
// Settings.txt tokens ("--render-scale", "2", ...) for the non-default values
// only, and the reverse; an unknown token is left for the caller.
std::vector<std::string> graphics_tokens(const GraphicsSettings& s);
// Applies one option (and its value, advancing `i`) from `args`; false if
// args[i] is not a graphics option.
bool apply_graphics_option(GraphicsSettings& s, const std::vector<std::string>& args, size_t& i);
// The option names the settings file keeps.
const std::vector<std::string>& graphics_option_names();
GraphicsSettings clamp_graphics(GraphicsSettings s);

// ---- Persistent VkPipelineCache file envelope (vendor/device/UUID validated) ----------------
struct PipelineCacheKey {
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::array<uint8_t, 16> uuid{};
  bool operator==(const PipelineCacheKey&) const = default;
};
std::vector<uint8_t> encode_pipeline_cache_file(const PipelineCacheKey& key, const uint8_t* payload, size_t size);
bool decode_pipeline_cache_file(const uint8_t* file_data, size_t file_size, const PipelineCacheKey& expected,
                                std::vector<uint8_t>& payload_out);

// ---- Split bindless descriptor limits (1x SAMPLER + SAMPLED_IMAGE[N]) -----------------------
// Replaces the old COMBINED_IMAGE_SAMPLER[4096] check so MoltenVK (Apple Silicon)
// and Android Turnip/Adreno GPUs with 16..2048 per-stage samplers qualify for WorldGpu.
struct SplitBindlessLimits {
  uint32_t max_per_stage_samplers = 0;
  uint32_t max_per_stage_sampled_images = 0;
  uint32_t max_set_sampled_images = 0;
};
bool split_bindless_supported(const SplitBindlessLimits& limits, uint32_t texture_slots = 4096);

}  // namespace carview::modern
