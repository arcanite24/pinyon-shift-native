// Photo Mode, F8/F12 PNG capture, supersampled accumulation, and Horizon Promo
// car spotting (Slice 10). Plain C++ + carview::ui so it can be unit-tested
// without Vulkan or SDL.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "drive/cameras.h"
#include "renderer.h"
#include "ui.h"

namespace carview {

// Stock FH1 PhotoModeCam limits (CameraSettings.ini) vs Pinyon Shift's
// Wider Photo Limits mod and Photo Camera Through Walls toggle.
struct PhotoLimits {
  double max_height = 2.0;           // stock: 2.0 m above anchor; wider: 30.0 m
  double ceiling_height = 10.0;      // stock: 10.0 m; wider: 50.0 m
  double cylinder_radius = 100.0;    // stock: 100.0 m; wider: 300.0 m
  double min_fov_deg = 20.0;         // stock: 20 deg; wider: 1 deg
  double max_fov_deg = 75.0;         // stock: 75 deg; wider: 85 deg
  double min_ground_clearance = 0.25;
  double max_roll_deg = 45.0;
  bool through_walls = false;

  static PhotoLimits from_settings(bool through_walls, bool wider_limits);
};

enum class PhotoPreset : int {
  kDefault = 0,
  kHorizonSunset,
  kMotorsportAction,
  kBlackAndWhite,
  kSepiaVintage,
  kVividFestival,
  kCount
};

const char* photo_preset_name(PhotoPreset p);

// Photo Mode optical & grading effects (0..100 slider domain where 50 is neutral
// for bipolar controls, matching FH1's 709_PHOTO_MODE).
struct PhotoEffects {
  float shutter_speed = 20.0f;    // 0..100 (higher = stronger wheel/radial motion blur)
  float aperture = 25.0f;         // 0..100 (higher = shallower depth of field / stronger bokeh)
  float focus_distance = 6.5f;    // 1.0..120.0 m
  float exposure = 50.0f;         // 0..100 (50 = neutral)
  float contrast = 50.0f;         // 0..100 (50 = 1.0)
  float colour = 55.0f;           // 0..100 (50 = 1.0 saturation, 0 = B&W)
  float brightness = 50.0f;       // 0..100 (50 = 0.0)
  float sepia = 0.0f;             // 0..100 (0 = off, 100 = full sepia)
  float vignette = 20.0f;         // 0..100 (0 = off, 100 = strong vignette)
  bool tracking_panning = false;  // false = Panning (background blur), true = Tracking (chassis locked)
  bool watermark = true;          // stamp Forza Horizon / Shift Engine watermark onto saved PNG
  bool hide_car = false;          // hide player car during Photo Mode for landscape shots
  PhotoPreset preset = PhotoPreset::kDefault;

  void apply_preset(PhotoPreset p);
  void reset() { apply_preset(PhotoPreset::kDefault); }

  // Applies the photo grading & optical controls on top of `base` PostSettings.
  PostSettings apply_to_post(const PostSettings& base) const;
  // Additional wheel angular speed boost for radial rim blur (rad/s).
  float wheel_blur_omega_boost() const;
};

struct PhotoCameraInput {
  float move_forward = 0.0f;  // -1..+1
  float move_right = 0.0f;    // -1..+1
  float move_up = 0.0f;       // -1..+1
  float look_yaw = 0.0f;      // -1..+1 (rad/s scaled)
  float look_pitch = 0.0f;    // -1..+1
  float zoom = 0.0f;          // -1..+1 (positive zooms in / narrows FOV)
  float roll = 0.0f;          // -1..+1 (degrees/s scaled)
};

// Free-orbit / free-fly Photo Mode camera anchored to the player's car.
class PhotoCamera {
 public:
  void reset(const std::array<double, 3>& anchor, double car_heading_rad, const PhotoLimits& limits);
  void set_limits(const PhotoLimits& limits);
  const PhotoLimits& limits() const { return limits_; }

  // Optional wall/obstacle raycast callback: given segment origin + unit dir + max_dist,
  // returns true and writes `hit_dist` if solid geometry blocks the camera.
  using WallRaycastFn = std::function<bool(const std::array<double, 3>& origin, const std::array<double, 3>& dir,
                                           double max_dist, double& hit_dist)>;

  void step(double dt, const PhotoCameraInput& in,
            const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below = {},
            const WallRaycastFn& wall_raycast = {});
  // Locks focus distance to distance between camera eye and `target`.
  float autofocus_distance(const std::array<double, 3>& target) const;

  drive::CameraView view() const;
  const std::array<double, 3>& eye() const { return eye_; }
  const std::array<double, 3>& anchor() const { return anchor_; }
  double yaw() const { return yaw_; }
  double pitch() const { return pitch_; }
  double roll_deg() const { return roll_deg_; }
  double fov_deg() const { return fov_deg_; }
  void set_fov_deg(double deg);
  void set_roll_deg(double deg);

 private:
  void clamp_to_limits(const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below,
                       const WallRaycastFn& wall_raycast = {});

  PhotoLimits limits_{};
  std::array<double, 3> anchor_{0, 0, 0};
  std::array<double, 3> eye_{0, 1.5, -5.0};
  double yaw_ = 0.0;       // radians, 0 looks +Z
  double pitch_ = -0.15;   // radians, negative looks down
  double roll_deg_ = 0.0;  // degrees
  double fov_deg_ = 45.0;  // vertical FOV degrees
};

// Multi-frame supersampled accumulation with Halton (2, 3) sub-pixel jitter.
class PhotoAccumulator {
 public:
  void begin(uint32_t width, uint32_t height, int target_samples = 4);
  void cancel();
  bool active() const { return active_; }
  bool complete() const { return active_ && count_ >= target_; }
  int sample_index() const { return count_; }
  int target_samples() const { return target_; }

  // Sub-pixel jitter in NDC units for the current sample (add to projection[8..9]
  // or translate clip x, y by jx * w, jy * w).
  void jitter_ndc(float& jx, float& jy) const;

  // Adds one RGBA8 frame of matching width x height.
  bool add_frame(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);
  // Resolves accumulated frames into averaged sRGB RGBA8.
  bool resolve(std::vector<uint8_t>& out_rgba, uint32_t& out_w, uint32_t& out_h);

 private:
  bool active_ = false;
  uint32_t width_ = 0, height_ = 0;
  int target_ = 4;
  int count_ = 0;
  std::vector<float> accum_rgb_;  // linear/gamma-averaged float RGB
};

// Horizon Promo car spotting candidate in world space.
struct PromoCarCandidate {
  int car_id = 0;
  std::string display_name;
  std::array<double, 3> world_pos{};
  double radius_m = 2.2;
  bool is_player = false;
};

struct PromoSpotResult {
  std::vector<int> visible_car_ids;
  std::vector<std::string> visible_names;
  std::vector<int> newly_spotted_ids;
  std::vector<std::string> newly_spotted_names;
  int64_t credits_awarded = 0;
  int total_spotted = 0;
  int total_cars = 240;
  bool milestone_reached = false;
};

// Tracks photographed car IDs (synced with DriveSave::promo_cars) and evaluates
// which cars are visible inside the camera frustum.
class HorizonPromoTracker {
 public:
  static constexpr int64_t kCreditsPerNewCar = 1000;
  static constexpr int64_t kMilestoneBonus = 10000;
  static constexpr int kMilestoneStep = 10;

  void load(const std::vector<int>& spotted_ids, int total_cars = 240);
  std::vector<int> save_ids() const;
  int spotted_count() const { return int(spotted_.size()); }
  int total_cars() const { return total_cars_; }
  bool is_spotted(int car_id) const { return spotted_.count(car_id) != 0; }

  // Evaluates visible cars using column-major Vulkan clip_from_world[16].
  // Updates spotted_ when `commit` is true and returns the spotting summary.
  PromoSpotResult evaluate(const float clip_from_world[16], const std::array<double, 3>& eye,
                           const std::vector<PromoCarCandidate>& candidates, bool commit = true);

 private:
  std::set<int> spotted_;
  int total_cars_ = 240;
};

// Optional retail/custom RGBA8 watermark image composited onto exported photos
// when non-empty; pass empty `rgba` to restore the built-in high-resolution
// antialiased "FORZA HORIZON" badge.
void set_photo_watermark_image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba);

// Composites a crisp, high-resolution antialiased Forza Horizon corner watermark onto RGBA8 pixels.
void stamp_photo_watermark(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);

// Decodes a saved photo (.png/.bmp/.jpg) via stb_image and box-downsamples it
// into an aspect-preserving RGBA8 thumbnail fitting within `max_w x max_h`.
bool load_photo_thumbnail(const std::filesystem::path& path, uint32_t max_w, uint32_t max_h,
                          std::vector<uint8_t>& out_rgba, uint32_t& out_w, uint32_t& out_h,
                          uint32_t* src_w = nullptr, uint32_t* src_h = nullptr);

// Writes an RGBA8 image to `<dir>/<prefix>-YYYYMMDD-HHMMSS[-N].png` using
// stb_image_write, creating `<dir>` if needed. Stamps the Horizon watermark when `add_watermark` is true.
bool write_photo_png(const std::filesystem::path& dir, const std::vector<uint8_t>& rgba, uint32_t width,
                     uint32_t height, const std::string& prefix, std::filesystem::path& out_path,
                     std::string* error = nullptr, bool add_watermark = false);

struct PhotoGalleryEntry {
  std::filesystem::path path;
  std::string filename;
  uint64_t size_bytes = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t thumb_width = 0;
  uint32_t thumb_height = 0;
  std::string thumb_key;
  std::array<float, 4> thumb_uv{};
  bool has_thumb = false;
  bool thumb_tried = false;
};

// Lists saved photos (.png/.bmp/.jpg/.jpeg) in `photos_dir` sorted newest-first.
std::vector<PhotoGalleryEntry> list_photo_gallery(const std::filesystem::path& photos_dir);

// Full Photo Mode state & HUD controller.
class PhotoModeController {
 public:
  // Optional `_&<hash>` string-table resolver (e.g. `carview::game_text`) for localized Photo Mode labels.
  static void set_text_resolver(std::function<std::string(const std::string&)> fn);
  enum EffectRow : int {
    kPreset = 0,
    kShutterSpeed,
    kShutterMode,
    kAperture,
    kFocusDistance,
    kExposure,
    kContrast,
    kColour,
    kBrightness,
    kSepia,
    kVignette,
    kZoomFov,
    kRollTilt,
    kWatermark,
    kHideCar,
    kRowCount
  };

  void enter(const std::array<double, 3>& car_pos, double car_heading_rad, const PhotoLimits& limits);
  void exit();
  bool active() const { return active_; }

  void set_limits(const PhotoLimits& limits) { camera_.set_limits(limits); }
  PhotoCamera& camera() { return camera_; }
  const PhotoCamera& camera() const { return camera_; }
  PhotoEffects& effects() { return effects_; }
  const PhotoEffects& effects() const { return effects_; }
  PhotoAccumulator& accumulator() { return accum_; }
  const PhotoAccumulator& accumulator() const { return accum_; }

  bool effects_open() const { return effects_open_; }
  void set_effects_open(bool open) { effects_open_ = open; }
  void toggle_effects() { effects_open_ = !effects_open_; }
  bool ui_hidden() const { return ui_hidden_; }
  void toggle_ui_hidden() { ui_hidden_ = !ui_hidden_; }
  void set_metric_units(bool metric) { metric_units_ = metric; }
  bool metric_units() const { return metric_units_; }
  int selected_row() const { return selected_row_; }
  int scroll_top() const { return scroll_top_; }
  static int visible_effect_rows(float h);
  void select_row(int row);

  void move_row(int delta);
  void adjust_row(int delta);
  void autofocus(const std::array<double, 3>& target);

  void step(double dt, const PhotoCameraInput& input,
            const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below = {},
            const PhotoCamera::WallRaycastFn& wall_raycast = {});

  // Saved Photo Gallery overlay (browse & delete saved screenshots).
  void open_gallery(const std::filesystem::path& photos_dir);
  void close_gallery() { gallery_open_ = false; }
  bool gallery_open() const { return gallery_open_; }
  void gallery_move(int delta);
  void gallery_select(int idx);
  bool gallery_delete_selected();
  const std::vector<PhotoGalleryEntry>& gallery_entries() const { return gallery_entries_; }
  int gallery_selected() const { return gallery_selected_; }
  bool gallery_has_preview() const { return gallery_preview_valid_; }
  uint32_t gallery_preview_width() const { return gallery_preview_w_; }
  uint32_t gallery_preview_height() const { return gallery_preview_h_; }

  // Begins a multi-frame supersampled capture (4x Halton jitter).
  void begin_capture(uint32_t width, uint32_t height, int samples = 4);
  void notify_photo_saved(const std::filesystem::path& saved_path, const PromoSpotResult& promo);
  void update_live_promo_preview(const PromoSpotResult& preview) { live_promo_ = preview; }

  const std::string& last_saved_label() const { return last_saved_label_; }
  const PromoSpotResult& last_promo() const { return last_promo_; }

  // Renders the Photo Mode viewfinder, effects drawer, Horizon Promo banner,
  // and control hints into `out`.
  void build_hud(ui::List& out, float w, float h) const;

 private:
  bool active_ = false;
  bool effects_open_ = true;
  bool ui_hidden_ = false;
  bool metric_units_ = false;  // miles by default (Q277)
  int selected_row_ = kAperture;
  mutable int scroll_top_ = 0;
  double time_ = 0.0;
  double flash_timer_ = 0.0;
  double banner_timer_ = 0.0;

  PhotoCamera camera_{};
  PhotoEffects effects_{};
  PhotoAccumulator accum_{};
  PromoSpotResult live_promo_{};
  PromoSpotResult last_promo_{};
  std::string last_saved_label_;

  // Saved Photo Gallery state.
  bool ensure_entry_thumbnail(size_t idx) const;
  bool ensure_selected_preview() const;

  bool gallery_open_ = false;
  std::filesystem::path gallery_dir_;
  mutable std::vector<PhotoGalleryEntry> gallery_entries_;
  int gallery_selected_ = 0;
  mutable std::filesystem::path gallery_preview_path_;
  mutable std::string gallery_preview_key_;
  mutable std::array<float, 4> gallery_preview_uv_{};
  mutable uint32_t gallery_preview_w_ = 0;
  mutable uint32_t gallery_preview_h_ = 0;
  mutable bool gallery_preview_valid_ = false;
};

}  // namespace carview
