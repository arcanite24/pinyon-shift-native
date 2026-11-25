// 24-hour Time-of-Day cycle, real-time sky asset parsing, TrackSettings post
// config, 16x16x16 3D colour-grading LUTs, and PostProcessingZones lookup.
//
// Spec: SPEC-SKY-AND-TOD (horizon1-recomp docs/specs/SKY_AND_TOD.md).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "fh1data/xds.h"

namespace world {

// One animation curve from TimeOfDay*.xml: sorted keys in [0, 1439] minutes.
struct TodCurve {
  struct Key {
    float time_min = 0.0f;
    float v[3] = {0.0f, 0.0f, 0.0f};
  };
  std::vector<Key> keys;

  float eval_scalar(float time_min, float fallback = 0.0f) const;
  std::array<float, 3> eval_vec3(float time_min, std::array<float, 3> fallback = {0.0f, 0.0f, 0.0f}) const;
};

// Parsed TimeOfDayA.xml / TimeOfDayB.xml / TimeOfDayNeutral.xml curves.
struct TodPreset {
  std::map<std::string, TodCurve> curves;

  bool load_xml(const std::string& xml_text, std::string* error = nullptr);
  bool load_file(const std::filesystem::path& path, std::string* error = nullptr);
  void merge_fallback(const TodPreset& fallback);

  float scalar(const std::string& name, float time_min, float fallback = 0.0f) const;
  std::array<float, 3> vec3(const std::string& name, float time_min,
                            std::array<float, 3> fallback = {0.0f, 0.0f, 0.0f}) const;
};

// All lighting, atmosphere, cloud, fog, and post curves evaluated at one moment.
struct TodState {
  float tod_sec = 43200.0f;  // [0, 86400)
  float tod_min = 720.0f;    // [0, 1440)

  // Sun / Moon directional body (game frame, unit vectors toward the body).
  std::array<float, 3> sun_dir{0.3848f, 0.8754f, 0.2926f};
  std::array<float, 3> sun_object_dir{0.3848f, 0.8754f, 0.2926f};
  std::array<float, 3> haze_dir{0.3848f, 0.8754f, 0.2926f};
  std::array<float, 3> sun_color{0.9695f, 0.8358f, 0.5840f};
  std::array<float, 3> car_sun_color{0.9695f, 0.8358f, 0.5840f};
  std::array<float, 3> sun_object_color{1.0f, 0.99f, 0.92f};
  float sun_object_size = 20.0f;
  float sun_object_alpha = 0.5f;
  bool is_moon = false;
  float shadow_intensity = 1.0f;  // +1 day, drops to -1 at night

  // Hemisphere and ambient lighting.
  std::array<float, 3> hl_top{0.1687f, 0.2229f, 0.2746f};
  std::array<float, 3> hl_bottom{0.1788f, 0.1729f, 0.1024f};
  std::array<float, 3> ambient_color{0.1717f, 0.2079f, 0.2229f};
  std::array<float, 3> car_hl_top{0.1687f, 0.2229f, 0.2746f};
  std::array<float, 3> car_hl_bottom{0.1788f, 0.1729f, 0.1024f};
  float ibl_ambient_scale = 1.0f;
  float ibl_direct_env = 1.3f;
  float ibl_direct_car = 0.62f;

  // Sky atmosphere gradient + elliptical haze + stars.
  float sky_gain = 2.0f;
  std::array<float, 3> atmo_top{0.007f, 0.034f, 0.097f};
  std::array<float, 3> atmo_bottom{0.48f, 0.91f, 0.997f};
  float atmo_color_power = 0.165f;
  float atmo_clamp_lo = 0.06f;
  float atmo_clamp_hi = 0.50f;
  std::array<float, 3> atmo_haze_color{0.246f, 0.472f, 0.497f};
  float atmo_haze_power = 5.0f;
  float atmo_haze_intensity = 0.21f;
  float atmo_haze_elliptical = 1.0f;
  float atmo_haze_alpha = 0.8f;
  float star_fade = 0.0f;

  // Far high-altitude clouds (Front + Back).
  std::array<float, 3> far_dense{0.39f, 0.39f, 0.44f};
  std::array<float, 3> far_thin{0.79f, 0.79f, 0.74f};
  float far_thin_intensity = 1.5f;
  float far_centre_alpha = 1.0f;
  float far_edge_alpha = 1.0f;
  float far_alpha_power = 2.0f;
  float far_coverage = 1.0f;
  std::array<float, 3> far_back_dense{0.39f, 0.39f, 0.44f};
  std::array<float, 3> far_back_thin{0.79f, 0.79f, 0.76f};
  float far_back_centre_alpha = 1.0f;
  float far_back_edge_alpha = 1.0f;
  float far_back_coverage = 1.0f;
  float far_back_speed_x = -50.0f;
  float far_back_speed_z = 40.0f;

  // Close lit dome clouds (Front + Back).
  std::array<float, 3> close_top_color{1.0f, 0.985f, 0.888f};
  std::array<float, 3> close_bottom_color{0.84f, 0.29f, 0.001f};
  std::array<float, 3> close_back_color{0.865f, 0.50f, 0.045f};
  std::array<float, 3> close_dense_color{0.063f, 0.113f, 0.136f};
  std::array<float, 3> close_thin_color{0.35f, 0.544f, 0.742f};
  std::array<float, 3> close_back_dense_color{0.063f, 0.113f, 0.136f};
  std::array<float, 3> close_back_thin_color{0.35f, 0.544f, 0.742f};
  std::array<float, 3> close_back_bottom_color{0.84f, 0.29f, 0.001f};
  float close_dense_intensity = 1.0f;
  float close_thin_intensity = 1.0f;
  float close_back_power = 8.0f;
  float close_back_intensity = 0.25f;
  float close_top_power = 2.0f;
  float close_top_intensity = 0.5f;
  float close_bottom_power = 2.0f;
  float close_bottom_intensity = 0.05f;
  float close_back_bottom_power = 1.0f;
  float close_back_bottom_intensity = 0.0f;
  float close_top_mask_power = 1.0f;
  float close_bottom_mask_power = 1.5f;
  float close_top_alpha = 1.0f;
  float close_bottom_alpha = 0.0f;
  float close_front_alpha = 1.0f;
  float close_back_alpha = 0.0f;
  float close_alpha_curve = 10.0f;
  float close_front_revs = 2.0f;
  float close_back_revs = -1.269f;

  // Dynamic reflection cubemap curves & TrackSettings scaling.
  std::array<float, 3> cube_atmo_haze_color{0.246f, 0.472f, 0.497f};
  float cube_atmo_haze_power = 4.0f;
  float cube_atmo_haze_intensity = 1.0f;
  float cube_atmo_haze_elliptical = 0.64f;
  std::array<float, 3> cube_sun_object_color{1.0f, 0.99f, 0.92f};
  float cube_sun_object_size = 10.0f;
  float cube_sky_saturation = 0.60f;
  float cube_refl_ground_scale = 1.0f;
  float cube_far_distance = 200.0f;

  // Shadow cascade ends & tint forwarded from TrackSettings.xml.
  float shadow_near = 25.0f;
  float shadow_far = 202.0f;
  std::array<float, 3> shadow_color{0.35f, 0.35f, 0.35f};

  // Atmospheric distance/height/scatter fog.
  std::array<float, 3> fog_color{0.091f, 0.1374f, 0.1446f};
  float fog_density = 0.03f;
  float fog_start = 975.0f;
  std::array<float, 3> fog_sun_scatter_color{0.223f, 0.223f, 0.223f};
  float fog_sun_scatter_power = 5.0f;
  float fog_sun_scatter_start = 0.0f;
  float fog_sun_scatter_rampup = 10000.0f;
  float fog_height_fade_amount = 0.9f;
  float fog_height_fade_start = 200.0f;
  float fog_height_fade_end = 4000.0f;
  float fog_sky_height_fade_start = 0.0f;
  float fog_sky_height_fade_end = 1500.0f;

  // Clock, night lights, and post-processing multipliers.
  float time_speed = 28.0f;
  float switch_on_lights = 0.0f;
  float day_night_post = 0.0f;
  float day_color_grade = 1.0f;
  float night_color_grade = 0.0f;
  float vignette_amount = 1.0f;
  std::array<float, 3> light_rays_color{1.0f, 0.9f, 0.7f};
  float light_rays_intensity = 0.0f;
  float bloom_scale_mult = 1.0f;
  float bloom_cutoff_mult = 1.0f;
  float bloom_lower_grad_mult = 1.0f;
  float bloom_upper_grad_mult = 1.0f;
  float bloom_persist_mult = 1.0f;
  float desat_amount_mult = 1.0f;
  std::array<float, 3> desat_luminosity_mult{1.0f, 1.0f, 1.0f};
  std::array<float, 3> desat_post_mod_mult{1.0f, 1.0f, 1.0f};
  float contrast_mult = 1.0f;
};

// Continuous 24-hour clock with Colorado's non-uniform TimeSpeed schedule
// (~30.46 real minutes per full day) and 20:00 A/B preset roll
// (SPEC-SKY-AND-TOD §2.1-2.3). Curves are evaluated raw at tod_sec: no
// temporal smoothing, and the A/B choice switches without a cross-fade.
struct TodClock {
  double tod_sec = 43200.0;      // [0, 86400)
  int day_count = 2;             // starts on Moon2.xds (full/gibbous)
  bool paused = false;
  double speed_scale = 1.0;      // 0 = frozen, 1 = 30.5m day, 2 = 15m, 0.5 = 61m, etc.
  bool use_preset_b = false;
  bool force_preset = false;     // when true, disables 20:00 random A/B roll
  // A requested transition (the original's time-of-day request): the clock
  // runs at `request_rate` game s / real s instead of the TimeSpeed curve,
  // never below `request_floor` (where it was asked from), until it reaches
  // `request_target`, where it stops and pauses.
  bool rate_override = false;
  double request_rate = 0.0;
  bool transition = false;
  double request_target = 0.0;
  double request_floor = 0.0;
  uint32_t rng_state = 0x51A7E3C1u;

  void set_hour(double hour);
  double hour() const { return tod_sec / 3600.0; }
  void step_hours(double delta_hours);
  // Asks the clock to run forward to `target_sec` (add 86,400 to pass
  // midnight) at `rate` game seconds per real second, then pause there.
  void request(double target_sec, double rate);
  // Advances the clock by `dt` real seconds using `curve_time_speed` (game s / real s).
  void advance(double dt, double curve_time_speed);
  // Lunar phase index [0..4] for Moon0..4.xds.
  int moon_phase() const { return ((day_count % 5) + 5) % 5; }
};

// Evaluates all curves of `preset` at `tod_sec`.
TodState evaluate_tod(const TodPreset& preset, double tod_sec);

// Track-wide post-processing, exposure, and shadow configuration from TrackSettings.xml.
struct TrackPostConfig {
  // Day and night Hable filmic tone-map curves.
  float hable_day[6] = {0.700001f, 0.038f, 0.634f, 0.220001f, 0.003f, 0.107f};
  float white_day = 1.0f;
  float hable_night[6] = {0.23f, 0.10f, 0.334f, 0.32f, 0.003f, 0.107f};
  float white_night = 11.0f;
  float filmic_exposure = 0.31f;

  // 4-key piecewise-linear adaptive exposure tables (SPEC-SKY-AND-TOD §5.2).
  float exterior_keys[4] = {0.0f, 4.0f, 13.0f, 65.0f};
  float exterior_ev[4] = {1.300f, 0.437f, 0.001f, -0.800f};
  float cockpit_keys[4] = {0.0f, 9.0f, 50.0f, 65.0f};
  float cockpit_ev[4] = {1.300f, -0.400f, -0.400f, -0.800f};
  float tonemap_delay = 0.075f;
  float exposure_min = -2.0f;
  float exposure_max = 6.0f;

  // Colour desaturation, post-tint, and track-wide color settings defaults.
  float desat_amount = 0.90f;
  float desat_luminosity[3] = {0.2126f, 0.7152f, 0.0722f};
  float desat_post_mod[3] = {1.04f, 1.02f, 1.00f};
  float exposure_bias = 0.0f;
  float contrast = 0.0f;
  float brightness = 0.0f;

  // Dynamic cubemap TrackSettings parameters (cubeSkySaturation / cubeReflGroundScale / Cubemap).
  float cube_sky_scale = 0.0f;
  float cube_sky_saturation = 0.60f;
  float cube_refl_ground_scale = 1.0f;
  float cube_far_distance = 200.0f;

  // Bloom defaults.
  float bloom_cutoff = 1.0f;
  float bloom_scale = 1.0f;
  float bloom_persist = 0.001f;
  float bloom_lower_grad = 1.0f;
  float bloom_upper_grad = 1.0f;
  float bloom_weights[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

  // Shadow cascade ends & tint (ShadowEnds / ShadowColor).
  float shadow_near = 25.0f;
  float shadow_far = 202.0f;
  float shadow_color[3] = {0.35f, 0.35f, 0.35f};

  // Sun disc angular bounds (SunSize).
  float sun_size_min = 10.0f;
  float sun_size_max = 20.0f;

  // Wind (sanitized to reject retail uninitialized z = -431602080.0f), grass shells,
  // crowd tints, and motion blur / DoF parameters (SPEC-VFX-AND-PARTICLES).
  float wind_dir[3] = {1.0f, 0.0f, 0.0f};
  float wind_amplitude = 0.20f;
  float wind_speed = 1.45f;
  float grass_height_scale = 1.30f;
  float grass_lod_dist = 4.0f;
  float grass_lod_fade_range = 3.0f;
  int grass_min_shells = 2;
  float grass_fade_start = 250.0f;
  float grass_fade_end = 280.0f;
  float motion_blur_amount = 0.0f;
  float detail_blur_mph_max = 60.0f;
  float dof_focal_distance = 4.0f;
  float dof_focal_length = 0.0f;
  float dof_focal_max_blur = 0.0f;
  float crowd_dark[3] = {0.501f, 0.521f, 0.541f};
  float crowd_light[3] = {1.5f, 1.5f, 1.5f};
  float crowd_sprite_distance = 60.0f;

  bool load_xml(const std::string& xml_text);
  bool load_file(const std::filesystem::path& path);

  // Camera motion-blur strength [0, 0.95] at a car speed (FEEL-31):
  // motionBlurAmount scaled by speed up to DetailBlurMPHForMaxBlur, with no
  // floor, so the free-roam baseline of 0 means no blur. The original's
  // dynamic modulation is not decoded (SPEC_QUESTIONS Q72).
  float motion_blur_strength(double speed_mps) const;

  // Evaluates the 4-key piecewise-linear EV curve (SPEC-SKY-AND-TOD §5.2).
  float evaluate_ev(float adapted_luminance, bool cockpit, float exposure_bias = 0.0f) const;
};

// 16x16x16 3D colour-grading volume LUT (RGBA8, X=R, Y=G, Z=B).
struct ColorLut16 {
  static constexpr uint32_t kSize = 16;
  static constexpr size_t kByteSize = kSize * kSize * kSize * 4;
  std::array<uint8_t, kByteSize> rgba{};

  static ColorLut16 identity();
  // Loads a 16,512-byte uncompressed 16x16x16 A8R8G8B8 DDS volume texture.
  bool load_dds(std::span<const uint8_t> bytes, std::string* error = nullptr);
  bool load_dds_file(const std::filesystem::path& path, std::string* error = nullptr);

  // Blends identity -> day LUT (day_amt) -> night LUT (night_amt) on CPU.
  static ColorLut16 blend(const ColorLut16& day_lut, const ColorLut16& night_lut,
                          float day_amt, float night_amt);

  // Trilinear software lookup for unit tests (in/out in [0, 1]).
  std::array<float, 3> sample(float r, float g, float b) const;
};

// Frame-rate-independent float-precision 3D LUT cross-fader. Eliminates uint8
// per-frame quantization snapping at high frame rates (120+ FPS) across
// PostProcessingZones boundaries and dawn/dusk grading transitions.
struct ColorLutBlender {
  bool initialized = false;
  std::array<float, ColorLut16::kByteSize> cur_f{};
  ColorLut16 current = ColorLut16::identity();

  void reset() { initialized = false; }
  void snap(const ColorLut16& target);
  // Advances `current` toward `target` with exponential time constant `tau_sec`.
  // Returns true if `current.rgba` changed and should be re-uploaded to the GPU.
  bool update(const ColorLut16& target, float dt, float tau_sec = 2.5f, bool force_snap = false);
  bool settled(const ColorLut16& target, float eps = 0.25f) const;
};

// Regional post-processing zone from PostProcessingZones_Safe.xml.
struct PostZone {
  struct Tri {
    float x[3] = {};
    float z[3] = {};
  };
  std::string name;
  std::string bloom_template;
  std::string color_grading_day;
  std::string color_grading_night;
  std::string color_settings;
  std::string fog_template;
  std::string vignette_top;
  std::string vignette_bottom;
  float lerp_time = 15.0f;
  std::vector<Tri> triangles;

  bool contains(float x, float z) const;
};

struct BloomTemplate {
  float cutoff = 1.0f;
  float scale = 1.0f;
  float persist = 0.001f;
  float lower_grad = 1.0f;
  float upper_grad = 1.0f;
  float weights[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};

struct ColorSettingsTemplate {
  float desat_amount = 0.9f;
  float desat_luminosity[3] = {0.2126f, 0.7152f, 0.0722f};
  float desat_post_mod[3] = {1.0f, 1.0f, 1.0f};
  float exposure_bias = 0.0f;
  float contrast = 0.0f;
  float brightness = 0.0f;
};

struct FogTemplate {
  float color[3] = {1.0f, 1.0f, 1.0f};
  float density = 1.0f;
  float start_distance = 1.0f;
  float sun_scatter_color[3] = {1.0f, 1.0f, 1.0f};
  float sun_scatter_power = 1.0f;
  float sun_scatter_start_distance = 0.0f;
  float sun_scatter_rampup_distance = 10000.0f;
  float height_fade_amount = 1.0f;
  float height_fade_start = 1.0f;
  float height_fade_end = 1.0f;
};

struct VignetteTemplate {
  float angle_deg = 0.0f;
  float pos_x = 0.0f;
  float pos_y = 0.0f;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float power = 2.0f;
  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float min_intensity = 0.25f;
  float max_intensity = 1.0f;
};

// Collection of Colorado's 16 post-processing zones + cached 3D LUTs & templates.
struct PostZoneSet {
  std::vector<PostZone> zones;
  std::map<std::string, ColorLut16> luts;  // lower-case filename -> 16^3 LUT
  std::map<std::string, BloomTemplate> bloom_templates;
  std::map<std::string, ColorSettingsTemplate> color_templates;
  std::map<std::string, FogTemplate> fog_templates;
  std::map<std::string, VignetteTemplate> vignette_templates;
  ColorLut16 fallback_day = ColorLut16::identity();
  ColorLut16 fallback_night = ColorLut16::identity();

  bool load(const std::filesystem::path& world_dir, const std::filesystem::path& media_dir,
            std::string* error = nullptr);
  // Returns matching zone index, or -1 if outside all 152 triangles.
  int find_zone(float game_x, float game_z) const;
  const ColorLut16& day_lut_for(int zone_index) const;
  const ColorLut16& night_lut_for(int zone_index) const;
  BloomTemplate bloom_for(int zone_index, const TrackPostConfig& defaults) const;
  ColorSettingsTemplate color_settings_for(int zone_index, const TrackPostConfig& defaults) const;
  FogTemplate fog_for(int zone_index) const;
  VignetteTemplate vignette_top_for(int zone_index) const;
  VignetteTemplate vignette_bottom_for(int zone_index) const;
};

// Smoothly interpolates regional PostZone templates over each zone's LerpTime.
struct PostZoneBlender {
  bool initialized = false;
  int active_zone = -1;
  BloomTemplate bloom{};
  ColorSettingsTemplate color{};
  FogTemplate fog{};
  VignetteTemplate vig_top{};
  VignetteTemplate vig_bottom{};

  void update(const PostZoneSet& set, const TrackPostConfig& defaults, float game_x, float game_z, float dt,
              bool snap = false);
};

// Stars.bin celestial catalog (9,096 stars, 16-byte big-endian float4).
struct Star {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float brightness = 0.0f;  // [0, 1]
};

bool parse_stars_bin(std::span<const uint8_t> bytes, std::vector<Star>& out, std::string* error = nullptr);

// Decoded assets from media/realtimesky.zip.
struct RealTimeSkyAssets {
  std::vector<Star> stars;
  fh1::RgbaImage sun;
  std::array<fh1::RgbaImage, 5> moons;
  fh1::RgbaImage dither;
  fh1::RgbaImage far_cloud_front;
  fh1::RgbaImage far_cloud_back;
  fh1::RgbaImage far_cloud_mask;
  fh1::RgbaImage close_alpha_front;
  fh1::RgbaImage close_alpha_back;
  fh1::RgbaImage close_ambient_front;
  fh1::RgbaImage close_ambient_back;
  fh1::RgbaImage close_tb_front;
  fh1::RgbaImage close_tb_back;

  bool loaded() const { return !stars.empty() && sun.width > 0 && close_alpha_front.width > 0; }
  bool load_zip(const std::filesystem::path& zip_path, std::string* error = nullptr);
};

// Convenience bundle holding all Colorado TOD + sky + post-zone data.
struct TodSystem {
  TodPreset preset_a;
  TodPreset preset_b;
  TodPreset preset_neutral;
  TrackPostConfig post_config;
  PostZoneSet zones;
  RealTimeSkyAssets sky_assets;
  bool loaded_tod = false;
  bool loaded_sky = false;

  bool load(const std::filesystem::path& world_dir, std::string* error = nullptr);
  const TodPreset& active_preset(const TodClock& clock) const;
  TodState evaluate(const TodClock& clock) const;
};

}  // namespace world
