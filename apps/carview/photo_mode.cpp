#include "photo_mode.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>

#include <stb_image.h>
#include <stb_image_write.h>

#include "fh1data/stringtables.h"
#include "scene_screen.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

std::function<std::string(const std::string&)>& photo_text_fn() {
  static std::function<std::string(const std::string&)> fn;
  return fn;
}

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  if (const auto& fn = photo_text_fn()) {
    const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
    const std::string ref = "_&" + std::to_string(packed);
    const std::string resolved = fn(ref);
    if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  }
  return fallback;
}

struct CustomWatermarkImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

CustomWatermarkImage& custom_watermark_image() {
  static CustomWatermarkImage img;
  return img;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

float halton(int index, int base) {
  float f = 1.0f;
  float r = 0.0f;
  int i = index;
  while (i > 0) {
    f /= float(base);
    r += f * float(i % base);
    i /= base;
  }
  return r;
}

float srgb_to_linear(uint8_t c) {
  const float x = float(c) / 255.0f;
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

uint8_t linear_to_srgb(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  const float s = x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
  return uint8_t(std::clamp(std::lround(s * 255.0f), 0l, 255l));
}

std::string timestamp_tag() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  return buf;
}

std::string format_credits(int64_t v) {
  std::string s = std::to_string(v);
  for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
  return s;
}

}  // namespace

PhotoLimits PhotoLimits::from_settings(bool through_walls, bool wider_limits) {
  PhotoLimits l;
  l.through_walls = through_walls;
  if (wider_limits) {
    // Pinyon Shift's photo_mode_limits mod (CameraSettings.ini).
    l.max_height = 30.0;
    l.ceiling_height = 50.0;
    l.cylinder_radius = 300.0;
    l.min_fov_deg = 1.0;
    l.max_fov_deg = 85.0;
  }
  return l;
}

const char* photo_preset_name(PhotoPreset p) {
  switch (p) {
    case PhotoPreset::kDefault: return "DEFAULT";
    case PhotoPreset::kHorizonSunset: return "HORIZON SUNSET";
    case PhotoPreset::kMotorsportAction: return "MOTORSPORT ACTION";
    case PhotoPreset::kBlackAndWhite: return "B&W CLASSIC";
    case PhotoPreset::kSepiaVintage: return "SEPIA VINTAGE";
    case PhotoPreset::kVividFestival: return "VIVID FESTIVAL";
    default: return "DEFAULT";
  }
}

void PhotoEffects::apply_preset(PhotoPreset p) {
  preset = p;
  shutter_speed = 20.0f;
  aperture = 25.0f;
  focus_distance = 6.5f;
  exposure = 50.0f;
  contrast = 50.0f;
  colour = 55.0f;
  brightness = 50.0f;
  sepia = 0.0f;
  vignette = 20.0f;

  switch (p) {
    case PhotoPreset::kDefault:
      break;
    case PhotoPreset::kHorizonSunset:
      shutter_speed = 25.0f;
      aperture = 45.0f;
      exposure = 56.0f;
      contrast = 58.0f;
      colour = 72.0f;
      brightness = 52.0f;
      sepia = 14.0f;
      vignette = 35.0f;
      break;
    case PhotoPreset::kMotorsportAction:
      shutter_speed = 82.0f;
      aperture = 40.0f;
      exposure = 52.0f;
      contrast = 62.0f;
      colour = 60.0f;
      vignette = 30.0f;
      break;
    case PhotoPreset::kBlackAndWhite:
      shutter_speed = 15.0f;
      aperture = 50.0f;
      exposure = 50.0f;
      contrast = 70.0f;
      colour = 0.0f;
      brightness = 50.0f;
      sepia = 0.0f;
      vignette = 45.0f;
      break;
    case PhotoPreset::kSepiaVintage:
      shutter_speed = 10.0f;
      aperture = 45.0f;
      exposure = 52.0f;
      contrast = 58.0f;
      colour = 35.0f;
      sepia = 85.0f;
      vignette = 55.0f;
      break;
    case PhotoPreset::kVividFestival:
      shutter_speed = 35.0f;
      aperture = 38.0f;
      exposure = 54.0f;
      contrast = 64.0f;
      colour = 85.0f;
      brightness = 52.0f;
      vignette = 25.0f;
      break;
    default:
      break;
  }
}

PostSettings PhotoEffects::apply_to_post(const PostSettings& base) const {
  PostSettings out = base;
  // Exposure: 50 is neutral (0 EV); 0..100 maps to [-1.5, +1.5] EV.
  const float ev_offset = (exposure - 50.0f) / 50.0f * 1.5f;
  out.exposure *= std::exp2(ev_offset);
  // Contrast: 50 -> 1.0, range [0.5, 1.8].
  const float contrast_scale =
      contrast >= 50.0f ? 1.0f + (contrast - 50.0f) / 50.0f * 0.8f : 0.5f + (contrast / 50.0f) * 0.5f;
  out.contrast = std::clamp(base.contrast * contrast_scale, 0.35f, 2.2f);
  // Brightness: 50 -> 0.0, range [-0.30, +0.30].
  out.brightness = std::clamp(base.brightness + (brightness - 50.0f) / 50.0f * 0.30f, -0.45f, 0.45f);
  // Colour saturation: 0 -> 0.0 (B&W), 50 -> 1.0, 100 -> 1.8.
  const float sat = colour <= 50.0f ? (colour / 50.0f) : (1.0f + (colour - 50.0f) / 50.0f * 0.8f);
  out.desaturate = std::clamp(sat, 0.0f, 1.9f);
  // Sepia: 0..100 -> 0..1.
  out.sepia = std::clamp(sepia / 100.0f, 0.0f, 1.0f);
  // Vignette: 0..100 -> 0..1 (disable paired world vignette so slider has clean control).
  out.paired_vignette = false;
  out.vignette_amount = std::clamp(vignette / 100.0f, 0.0f, 1.0f);
  // Aperture -> Depth of Field (post.frag Poisson bokeh).
  const float ap = std::clamp(aperture / 100.0f, 0.0f, 1.0f);
  out.dof = ap > 0.01f;
  out.dof_focal_distance = std::clamp(focus_distance, 1.5f, 120.0f);
  out.dof_focal_range = std::clamp(48.0f - ap * 38.0f, 6.0f, 50.0f);
  out.dof_max_blur = ap * 0.92f;
  // Shutter speed -> radial motion blur (preserving subject car via depth mask).
  const float sh = std::clamp(shutter_speed / 100.0f, 0.0f, 1.0f);
  out.motion_blur = sh > 0.02f;
  out.motion_blur_directional = false;  // the still camera has no motion to follow
  out.motion_blur_center[0] = 0.5f;
  out.motion_blur_center[1] = 0.44f;
  // Panning mode emphasizes background streak with tight car protection; Tracking mode locks stronger car-relative blur.
  out.motion_blur_strength = sh * (tracking_panning ? 0.95f : 0.72f);
  out.motion_blur_car_protect =
      tracking_panning ? std::clamp(focus_distance + 2.5f, 5.0f, 28.0f) : std::clamp(focus_distance + 1.2f, 3.5f, 22.0f);
  return out;
}

float PhotoEffects::wheel_blur_omega_boost() const {
  const float mult = tracking_panning ? 82.0f : 58.0f;
  return std::clamp(shutter_speed / 100.0f, 0.0f, 1.0f) * mult;
}

void PhotoCamera::reset(const std::array<double, 3>& anchor, double car_heading_rad, const PhotoLimits& limits) {
  limits_ = limits;
  anchor_ = anchor;
  // Hero 3/4 front-left angle looking back toward the car.
  const double cam_angle = car_heading_rad + 0.65;
  const double dist = 5.4;
  eye_[0] = anchor[0] + std::sin(cam_angle) * dist;
  eye_[1] = anchor[1] + 1.25;
  eye_[2] = anchor[2] + std::cos(cam_angle) * dist;
  const double dx = anchor[0] - eye_[0];
  const double dy = (anchor[1] + 0.65) - eye_[1];
  const double dz = anchor[2] - eye_[2];
  yaw_ = std::atan2(dx, dz);
  pitch_ = std::atan2(dy, std::hypot(dx, dz));
  roll_deg_ = 0.0;
  fov_deg_ = std::clamp(42.0, limits_.min_fov_deg, limits_.max_fov_deg);
}

void PhotoCamera::set_limits(const PhotoLimits& limits) {
  limits_ = limits;
  fov_deg_ = std::clamp(fov_deg_, limits_.min_fov_deg, limits_.max_fov_deg);
  roll_deg_ = std::clamp(roll_deg_, -limits_.max_roll_deg, limits_.max_roll_deg);
}

void PhotoCamera::set_fov_deg(double deg) { fov_deg_ = std::clamp(deg, limits_.min_fov_deg, limits_.max_fov_deg); }

void PhotoCamera::set_roll_deg(double deg) { roll_deg_ = std::clamp(deg, -limits_.max_roll_deg, limits_.max_roll_deg); }

void PhotoCamera::clamp_to_limits(
    const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below,
    const WallRaycastFn& wall_raycast) {
  // Cylinder radius around anchor.
  const double dx = eye_[0] - anchor_[0];
  const double dz = eye_[2] - anchor_[2];
  const double r = std::hypot(dx, dz);
  if (r > limits_.cylinder_radius && r > 1e-6) {
    const double scale = limits_.cylinder_radius / r;
    eye_[0] = anchor_[0] + dx * scale;
    eye_[2] = anchor_[2] + dz * scale;
  }

  // Height limits above anchor (and ground clearance unless through_walls is enabled).
  double min_y = anchor_[1] - 5.0;
  double max_y = anchor_[1] + limits_.max_height;
  if (!limits_.through_walls && ground_below) {
    double gy = 0.0;
    if (ground_below(eye_[0], eye_[1] + 5.0, eye_[2], gy)) {
      min_y = std::max(min_y, gy + limits_.min_ground_clearance);
      max_y = std::min(anchor_[1] + limits_.ceiling_height, gy + limits_.max_height);
    }
  } else if (limits_.through_walls) {
    min_y = anchor_[1] - 25.0;
    max_y = anchor_[1] + limits_.ceiling_height;
  }
  if (max_y < min_y) max_y = min_y;
  eye_[1] = std::clamp(eye_[1], min_y, max_y);

  // Wall/building collision sweep from anchor pivot to eye when through_walls is OFF.
  if (!limits_.through_walls && wall_raycast) {
    const std::array<double, 3> orig = {anchor_[0], anchor_[1] + 0.9, anchor_[2]};
    const std::array<double, 3> delta = {eye_[0] - orig[0], eye_[1] - orig[1], eye_[2] - orig[2]};
    const double dist = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    if (dist > 0.6) {
      const std::array<double, 3> dir = {delta[0] / dist, delta[1] / dist, delta[2] / dist};
      double hit_d = 0.0;
      if (wall_raycast(orig, dir, dist, hit_d) && hit_d > 0.35 && hit_d < dist) {
        const double safe_d = std::max(0.35, hit_d - 0.35);
        eye_[0] = orig[0] + dir[0] * safe_d;
        eye_[1] = std::max(min_y, orig[1] + dir[1] * safe_d);
        eye_[2] = orig[2] + dir[2] * safe_d;
      }
    }
  }
}

void PhotoCamera::step(double dt, const PhotoCameraInput& in,
                       const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below,
                       const WallRaycastFn& wall_raycast) {
  dt = std::clamp(dt, 0.0, 0.1);
  if (dt <= 0.0) return;

  constexpr double kMoveSpeed = 7.5;     // m/s
  constexpr double kElevSpeed = 4.5;     // m/s
  constexpr double kLookSpeed = 1.65;    // rad/s
  constexpr double kZoomSpeed = 28.0;    // deg/s
  constexpr double kRollSpeed = 32.0;    // deg/s

  yaw_ += double(in.look_yaw) * kLookSpeed * dt;
  pitch_ = std::clamp(pitch_ + double(in.look_pitch) * kLookSpeed * dt, -1.35, 1.35);
  set_fov_deg(fov_deg_ - double(in.zoom) * kZoomSpeed * dt);
  set_roll_deg(roll_deg_ + double(in.roll) * kRollSpeed * dt);

  const double cp = std::cos(pitch_);
  const std::array<double, 3> fwd = {std::sin(yaw_) * cp, std::sin(pitch_), std::cos(yaw_) * cp};
  const std::array<double, 3> right = {std::cos(yaw_), 0.0, -std::sin(yaw_)};

  const double move_scale = limits_.cylinder_radius > 150.0 ? 1.8 : 1.0;
  for (int i = 0; i < 3; ++i) {
    eye_[i] += (fwd[i] * double(in.move_forward) + right[i] * double(in.move_right)) * kMoveSpeed * move_scale * dt;
  }
  eye_[1] += double(in.move_up) * kElevSpeed * move_scale * dt;

  clamp_to_limits(ground_below, wall_raycast);
}

float PhotoCamera::autofocus_distance(const std::array<double, 3>& target) const {
  const double dx = target[0] - eye_[0];
  const double dy = target[1] - eye_[1];
  const double dz = target[2] - eye_[2];
  return float(std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz), 1.5, 120.0));
}

drive::CameraView PhotoCamera::view() const {
  drive::CameraView v;
  v.eye = eye_;
  const double cp = std::cos(pitch_);
  v.forward = {std::sin(yaw_) * cp, std::sin(pitch_), std::cos(yaw_) * cp};
  // Compute rolled up vector around forward axis.
  const std::array<double, 3> right_base = {std::cos(yaw_), 0.0, -std::sin(yaw_)};
  // up_base = cross(forward, right_base) in right-handed? Let's match RH: up = right x forward -> check:
  // forward=(0,0,1), right=(1,0,0): right x forward = (0,-1,0), forward x right = (0,1,0).
  const std::array<double, 3> up_base = {
      v.forward[1] * right_base[2] - v.forward[2] * right_base[1],
      v.forward[2] * right_base[0] - v.forward[0] * right_base[2],
      v.forward[0] * right_base[1] - v.forward[1] * right_base[0],
  };
  const double rr = roll_deg_ * kDegToRad;
  const double cr = std::cos(rr), sr = std::sin(rr);
  for (int i = 0; i < 3; ++i) v.up[i] = up_base[i] * cr + right_base[i] * sr;
  const double ulen = std::sqrt(v.up[0] * v.up[0] + v.up[1] * v.up[1] + v.up[2] * v.up[2]);
  if (ulen > 1e-6)
    for (int i = 0; i < 3; ++i) v.up[i] /= ulen;
  v.fov_degrees = fov_deg_;
  v.near_clip = 0.2;
  v.mode = drive::CameraMode::kFollowLow;
  v.interior = false;
  return v;
}

void PhotoAccumulator::begin(uint32_t width, uint32_t height, int target_samples) {
  width_ = width;
  height_ = height;
  target_ = std::clamp(target_samples, 1, 16);
  count_ = 0;
  accum_rgb_.assign(size_t(width) * height * 3, 0.0f);
  active_ = width > 0 && height > 0;
}

void PhotoAccumulator::cancel() {
  active_ = false;
  count_ = 0;
  accum_rgb_.clear();
}

void PhotoAccumulator::jitter_ndc(float& jx, float& jy) const {
  if (!active_ || width_ == 0 || height_ == 0 || target_ <= 1) {
    jx = 0.0f;
    jy = 0.0f;
    return;
  }
  const float hx = halton(count_ + 1, 2) - 0.5f;
  const float hy = halton(count_ + 1, 3) - 0.5f;
  jx = (hx * 2.0f) / float(width_);
  jy = (hy * 2.0f) / float(height_);
}

bool PhotoAccumulator::add_frame(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  if (!active_ || width != width_ || height != height_) return false;
  const size_t pixels = size_t(width) * height;
  if (rgba.size() < pixels * 4) return false;
  for (size_t i = 0; i < pixels; ++i) {
    accum_rgb_[i * 3 + 0] += srgb_to_linear(rgba[i * 4 + 0]);
    accum_rgb_[i * 3 + 1] += srgb_to_linear(rgba[i * 4 + 1]);
    accum_rgb_[i * 3 + 2] += srgb_to_linear(rgba[i * 4 + 2]);
  }
  ++count_;
  return true;
}

bool PhotoAccumulator::resolve(std::vector<uint8_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) {
  if (!active_ || count_ <= 0) return false;
  const size_t pixels = size_t(width_) * height_;
  const float inv = 1.0f / float(count_);
  out_rgba.resize(pixels * 4);
  for (size_t i = 0; i < pixels; ++i) {
    out_rgba[i * 4 + 0] = linear_to_srgb(accum_rgb_[i * 3 + 0] * inv);
    out_rgba[i * 4 + 1] = linear_to_srgb(accum_rgb_[i * 3 + 1] * inv);
    out_rgba[i * 4 + 2] = linear_to_srgb(accum_rgb_[i * 3 + 2] * inv);
    out_rgba[i * 4 + 3] = 255;
  }
  out_w = width_;
  out_h = height_;
  cancel();
  return true;
}

void HorizonPromoTracker::load(const std::vector<int>& spotted_ids, int total_cars) {
  spotted_.clear();
  for (int id : spotted_ids)
    if (id > 0) spotted_.insert(id);
  total_cars_ = std::max(1, total_cars);
}

std::vector<int> HorizonPromoTracker::save_ids() const {
  return std::vector<int>(spotted_.begin(), spotted_.end());
}

PromoSpotResult HorizonPromoTracker::evaluate(const float m[16], const std::array<double, 3>& eye,
                                              const std::vector<PromoCarCandidate>& candidates, bool commit) {
  PromoSpotResult res;
  res.total_cars = total_cars_;
  const int before_count = int(spotted_.size());
  std::set<int> seen_this_frame;

  for (const auto& c : candidates) {
    if (c.car_id <= 0 || seen_this_frame.count(c.car_id)) continue;
    const double dx = c.world_pos[0] - eye[0];
    const double dy = (c.world_pos[1] + 0.7) - eye[1];
    const double dz = c.world_pos[2] - eye[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.5 || dist > 120.0) continue;

    // Project car centre through column-major clip_from_world matrix.
    const float px = float(c.world_pos[0]);
    const float py = float(c.world_pos[1] + 0.7);
    const float pz = float(c.world_pos[2]);
    const float cx = m[0] * px + m[4] * py + m[8] * pz + m[12];
    const float cy = m[1] * px + m[5] * py + m[9] * pz + m[13];
    const float cz = m[2] * px + m[6] * py + m[10] * pz + m[14];
    const float cw = m[3] * px + m[7] * py + m[11] * pz + m[15];
    if (cw <= 0.05f) continue;

    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    const float ndc_z = cz / cw;
    if (std::fabs(ndc_x) > 0.94f || std::fabs(ndc_y) > 0.94f) continue;
    if (ndc_z < -0.01f || ndc_z > 1.01f) continue;

    // Ensure the car spans a reasonable angular size in the frame (~1.2 degrees).
    const double ang_radius = c.radius_m / dist;
    if (ang_radius < 0.018) continue;

    seen_this_frame.insert(c.car_id);
    res.visible_car_ids.push_back(c.car_id);
    res.visible_names.push_back(c.display_name.empty() ? ("CAR #" + std::to_string(c.car_id)) : c.display_name);

    if (!spotted_.count(c.car_id)) {
      res.newly_spotted_ids.push_back(c.car_id);
      res.newly_spotted_names.push_back(res.visible_names.back());
      res.credits_awarded += kCreditsPerNewCar;
      if (commit) spotted_.insert(c.car_id);
    }
  }

  const int after_count = commit ? int(spotted_.size()) : before_count + int(res.newly_spotted_ids.size());
  res.total_spotted = after_count;
  const int milestones_before = before_count / kMilestoneStep;
  const int milestones_after = after_count / kMilestoneStep;
  if (milestones_after > milestones_before) {
    res.milestone_reached = true;
    res.credits_awarded += int64_t(milestones_after - milestones_before) * kMilestoneBonus;
  }
  return res;
}

namespace {

void blend_px_rgba(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int x, int y, uint8_t r, uint8_t g, uint8_t b,
                   uint8_t a) {
  if (x < 0 || y < 0 || x >= int(w) || y >= int(h) || a == 0) return;
  const size_t i = (size_t(y) * w + size_t(x)) * 4;
  const uint32_t alpha = a;
  const uint32_t inv = 255u - alpha;
  rgba[i + 0] = uint8_t((rgba[i + 0] * inv + r * alpha + 127u) / 255u);
  rgba[i + 1] = uint8_t((rgba[i + 1] * inv + g * alpha + 127u) / 255u);
  rgba[i + 2] = uint8_t((rgba[i + 2] * inv + b * alpha + 127u) / 255u);
  rgba[i + 3] = 255;
}

void composite_custom_watermark(std::vector<uint8_t>& dst, uint32_t dst_w, uint32_t dst_h,
                                const CustomWatermarkImage& src) {
  const float target_h = std::clamp(float(dst_h) * 0.085f, 24.0f, float(dst_h) * 0.25f);
  const float aspect = float(src.width) / float(std::max(1u, src.height));
  const uint32_t out_h = std::max(1u, uint32_t(std::lround(target_h)));
  const uint32_t out_w = std::clamp(uint32_t(std::lround(target_h * aspect)), 1u, std::max(1u, dst_w - 16u));
  const int margin = std::max(10, int(std::lround(float(dst_h) * 0.025f)));
  const int x0 = int(dst_w) - int(out_w) - margin;
  const int y0 = int(dst_h) - int(out_h) - margin;
  if (x0 < 0 || y0 < 0) return;

  for (uint32_t dy = 0; dy < out_h; ++dy) {
    const float sy = (float(dy) + 0.5f) * float(src.height) / float(out_h) - 0.5f;
    const int y_lo = std::clamp(int(std::floor(sy)), 0, int(src.height) - 1);
    const int y_hi = std::clamp(y_lo + 1, 0, int(src.height) - 1);
    const float fy = std::clamp(sy - float(y_lo), 0.0f, 1.0f);
    for (uint32_t dx = 0; dx < out_w; ++dx) {
      const float sx = (float(dx) + 0.5f) * float(src.width) / float(out_w) - 0.5f;
      const int x_lo = std::clamp(int(std::floor(sx)), 0, int(src.width) - 1);
      const int x_hi = std::clamp(x_lo + 1, 0, int(src.width) - 1);
      const float fx = std::clamp(sx - float(x_lo), 0.0f, 1.0f);
      const uint8_t* p00 = &src.rgba[(size_t(y_lo) * src.width + size_t(x_lo)) * 4];
      const uint8_t* p10 = &src.rgba[(size_t(y_lo) * src.width + size_t(x_hi)) * 4];
      const uint8_t* p01 = &src.rgba[(size_t(y_hi) * src.width + size_t(x_lo)) * 4];
      const uint8_t* p11 = &src.rgba[(size_t(y_hi) * src.width + size_t(x_hi)) * 4];
      const float w00 = (1.0f - fx) * (1.0f - fy);
      const float w10 = fx * (1.0f - fy);
      const float w01 = (1.0f - fx) * fy;
      const float w11 = fx * fy;
      const uint8_t r = uint8_t(std::clamp(std::lround(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11), 0l, 255l));
      const uint8_t g = uint8_t(std::clamp(std::lround(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11), 0l, 255l));
      const uint8_t b = uint8_t(std::clamp(std::lround(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11), 0l, 255l));
      const uint8_t a = uint8_t(std::clamp(std::lround(p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11), 0l, 255l));
      blend_px_rgba(dst, dst_w, dst_h, x0 + int(dx), y0 + int(dy), r, g, b, a);
    }
  }
}

void downsample_box_rgba(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                         std::vector<uint8_t>& out) {
  out.resize(size_t(dw) * dh * 4);
  if (sw == dw && sh == dh) {
    std::memcpy(out.data(), src, out.size());
    return;
  }
  const float sx_scale = float(sw) / float(dw);
  const float sy_scale = float(sh) / float(dh);
  for (uint32_t dy = 0; dy < dh; ++dy) {
    const float fy0 = float(dy) * sy_scale;
    const float fy1 = std::min(float(sh), float(dy + 1) * sy_scale);
    const uint32_t y0 = std::min(sh - 1u, uint32_t(std::floor(fy0)));
    const uint32_t y1 = std::min(sh, std::max(y0 + 1u, uint32_t(std::ceil(fy1))));
    for (uint32_t dx = 0; dx < dw; ++dx) {
      const float fx0 = float(dx) * sx_scale;
      const float fx1 = std::min(float(sw), float(dx + 1) * sx_scale);
      const uint32_t x0 = std::min(sw - 1u, uint32_t(std::floor(fx0)));
      const uint32_t x1 = std::min(sw, std::max(x0 + 1u, uint32_t(std::ceil(fx1))));
      double acc[4] = {0.0, 0.0, 0.0, 0.0};
      double wsum = 0.0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const double wy = std::max(0.0, double(std::min(fy1, float(sy + 1)) - std::max(fy0, float(sy))));
        const uint8_t* row = src + size_t(sy) * sw * 4;
        for (uint32_t sx = x0; sx < x1; ++sx) {
          const double wx = std::max(0.0, double(std::min(fx1, float(sx + 1)) - std::max(fx0, float(sx))));
          const double w = wx * wy;
          const uint8_t* p = row + size_t(sx) * 4;
          acc[0] += double(p[0]) * w;
          acc[1] += double(p[1]) * w;
          acc[2] += double(p[2]) * w;
          acc[3] += double(p[3]) * w;
          wsum += w;
        }
      }
      uint8_t* q = &out[(size_t(dy) * dw + size_t(dx)) * 4];
      if (wsum > 1e-9) {
        for (int c = 0; c < 4; ++c) q[c] = uint8_t(std::clamp(std::lround(acc[c] / wsum), 0l, 255l));
      } else {
        const uint8_t* p = src + (size_t(y0) * sw + size_t(x0)) * 4;
        std::memcpy(q, p, 4);
      }
    }
  }
}

}  // namespace

void set_photo_watermark_image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba) {
  auto& wm = custom_watermark_image();
  if (width == 0 || height == 0 || rgba.size() < size_t(width) * height * 4) {
    wm = CustomWatermarkImage{};
    return;
  }
  wm.width = width;
  wm.height = height;
  wm.rgba = std::move(rgba);
}

void stamp_photo_watermark(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  if (width < 120 || height < 60 || rgba.size() < size_t(width) * height * 4) return;
  const auto& custom_wm = custom_watermark_image();
  if (custom_wm.width > 0 && custom_wm.height > 0 &&
      custom_wm.rgba.size() >= size_t(custom_wm.width) * custom_wm.height * 4) {
    composite_custom_watermark(rgba, width, height, custom_wm);
    return;
  }

  // Smooth, resolution-independent antialiased "FORZA HORIZON" corner badge
  // rasterized via carview::ui SDF/vector primitives directly over the bottom-right ROI.
  const float s = std::clamp(float(height) / 720.0f, 0.45f, 4.0f);
  const bool use_vfont = !ui::gpu_atlas_mode() && ui::vector_fonts_loaded();
  const ui::Font main_font = use_vfont ? ui::Font::kE : ui::Font::kStroke;
  const ui::Font sub_font = use_vfont ? ui::Font::kB : ui::Font::kStroke;

  const std::string title_txt = "FORZA HORIZON";
  const std::string sub_txt = "SHIFT ENGINE";

  ui::TextStyle title_ts{};
  title_ts.font = main_font;
  title_ts.size = 18.0f * s;
  title_ts.weight = 0.135f;
  title_ts.slant = use_vfont ? 0.0f : 0.16f;
  title_ts.tracking = 0.06f;
  title_ts.color = ui::rgba(248, 250, 252, 238);

  ui::TextStyle sub_ts{};
  sub_ts.font = sub_font;
  sub_ts.size = 10.5f * s;
  sub_ts.weight = 0.115f;
  sub_ts.slant = use_vfont ? 0.0f : 0.08f;
  sub_ts.tracking = 0.08f;
  sub_ts.color = ui::rgba(218, 255, 0, 224);

  const float title_w = ui::List::measure(title_txt, title_ts);
  const float sub_w = ui::List::measure(sub_txt, sub_ts);
  const float shadow_pad = 10.0f * s;
  const float stripe_w = 5.0f * s;
  const float pad_l = 12.0f * s;
  const float gap_w = 14.0f * s;
  const float pad_r = 14.0f * s;
  const float badge_w = stripe_w + pad_l + title_w + gap_w + sub_w + pad_r;
  const float badge_h = 34.0f * s;

  const uint32_t roi_w = std::min(width, std::max(1u, uint32_t(std::ceil(badge_w + shadow_pad * 2.0f))));
  const uint32_t roi_h = std::min(height, std::max(1u, uint32_t(std::ceil(badge_h + shadow_pad * 2.0f))));
  const int margin_x = std::max(8, int(std::lround(18.0f * s)));
  const int margin_y = std::max(8, int(std::lround(14.0f * s)));
  const int x0 = int(width) - int(roi_w) - margin_x;
  const int y0 = int(height) - int(roi_h) - margin_y;
  if (x0 < 0 || y0 < 0) return;

  // Copy target ROI from the exported photo so ui::rasterize blends smoothly over true scene pixels.
  std::vector<uint8_t> roi(size_t(roi_w) * roi_h * 4);
  for (uint32_t y = 0; y < roi_h; ++y) {
    const uint8_t* src_row = &rgba[(size_t(y0 + int(y)) * width + size_t(x0)) * 4];
    std::memcpy(&roi[size_t(y) * roi_w * 4], src_row, size_t(roi_w) * 4);
  }

  const float bx0 = shadow_pad;
  const float by0 = shadow_pad;
  const float bx1 = std::min(float(roi_w) - shadow_pad * 0.5f, bx0 + badge_w);
  const float by1 = std::min(float(roi_h) - shadow_pad * 0.5f, by0 + badge_h);

  ui::List wl;
  // Soft drop shadow + translucent charcoal badge with Forza Horizon pink accent bar and hairline.
  wl.shadow(bx0, by0 + 1.5f * s, bx1, by1 + 1.5f * s, ui::rgba(0, 0, 0, 125), 3.0f * s, 7.0f * s);
  wl.rect(bx0, by0, bx1, by1, ui::rgba(14, 15, 20, 182), 2.5f * s, ui::rgba(10, 11, 15, 205));
  wl.rect(bx0, by0, bx0 + stripe_w, by1, ui4::kPink, 1.5f * s);
  wl.rect(bx0 + stripe_w, by1 - 2.0f * s, bx1, by1, ui::fade(ui4::kPink, 0.78));
  wl.frame(bx0, by0, bx1, by1, ui::rgba(255, 255, 255, 36), std::max(1.0f, 1.0f * s), 2.5f * s);

  const float tx = bx0 + stripe_w + pad_l;
  const float ty = by0 + 0.5f * (badge_h - title_ts.size) - 0.5f * s;
  ui::TextStyle title_sh = title_ts;
  title_sh.color = ui::rgba(0, 0, 0, 150);
  wl.text(tx + 1.2f * s, ty + 1.4f * s, title_txt, title_sh);
  wl.text(tx, ty, title_txt, title_ts);

  // Slanted Horizon pink divider between wordmark and engine tag.
  const float div_x = tx + title_w + 0.5f * gap_w;
  wl.line(div_x - 2.5f * s, by1 - 7.5f * s, div_x + 2.5f * s, by0 + 7.5f * s, std::max(1.4f, 1.8f * s), ui4::kPink);

  const float sx = tx + title_w + gap_w;
  const float sy = by0 + 0.5f * (badge_h - sub_ts.size);
  ui::TextStyle sub_sh = sub_ts;
  sub_sh.color = ui::rgba(0, 0, 0, 145);
  wl.text(sx + 1.0f * s, sy + 1.1f * s, sub_txt, sub_sh);
  wl.text(sx, sy, sub_txt, sub_ts);

  ui::rasterize(wl.items(), roi, roi_w, roi_h);

  for (uint32_t y = 0; y < roi_h; ++y) {
    uint8_t* dst_row = &rgba[(size_t(y0 + int(y)) * width + size_t(x0)) * 4];
    const uint8_t* src_row = &roi[size_t(y) * roi_w * 4];
    for (uint32_t x = 0; x < roi_w; ++x) {
      dst_row[x * 4 + 0] = src_row[x * 4 + 0];
      dst_row[x * 4 + 1] = src_row[x * 4 + 1];
      dst_row[x * 4 + 2] = src_row[x * 4 + 2];
      dst_row[x * 4 + 3] = 255;
    }
  }
}

bool load_photo_thumbnail(const std::filesystem::path& path, uint32_t max_w, uint32_t max_h,
                          std::vector<uint8_t>& out_rgba, uint32_t& out_w, uint32_t& out_h, uint32_t* src_w,
                          uint32_t* src_h) {
  out_rgba.clear();
  out_w = 0;
  out_h = 0;
  if (path.empty() || max_w == 0 || max_h == 0) return false;
  int iw = 0, ih = 0, ic = 0;
  unsigned char* decoded = stbi_load(path.string().c_str(), &iw, &ih, &ic, 4);
  if (!decoded || iw <= 0 || ih <= 0) {
    if (decoded) stbi_image_free(decoded);
    return false;
  }
  const uint32_t sw = uint32_t(iw);
  const uint32_t sh = uint32_t(ih);
  if (src_w) *src_w = sw;
  if (src_h) *src_h = sh;

  const double scale = std::min({1.0, double(max_w) / double(sw), double(max_h) / double(sh)});
  out_w = std::max(1u, uint32_t(std::lround(double(sw) * scale)));
  out_h = std::max(1u, uint32_t(std::lround(double(sh) * scale)));
  downsample_box_rgba(decoded, sw, sh, out_w, out_h, out_rgba);
  stbi_image_free(decoded);
  return true;
}

bool write_photo_png(const std::filesystem::path& dir, const std::vector<uint8_t>& rgba, uint32_t width,
                     uint32_t height, const std::string& prefix, std::filesystem::path& out_path, std::string* error,
                     bool add_watermark) {
  if (width == 0 || height == 0 || rgba.size() < size_t(width) * height * 4) {
    if (error) *error = "invalid image buffer";
    return false;
  }
  std::vector<uint8_t> stamped;
  const uint8_t* pixels = rgba.data();
  if (add_watermark) {
    stamped = rgba;
    stamp_photo_watermark(stamped, width, height);
    pixels = stamped.data();
  }
  std::error_code ec;
  if (!dir.empty()) std::filesystem::create_directories(dir, ec);
  const std::string tag = timestamp_tag();
  const std::string pfx = prefix.empty() ? "photo" : prefix;
  std::filesystem::path candidate = dir / (pfx + "-" + tag + ".png");
  for (int seq = 1; std::filesystem::exists(candidate, ec) && seq < 999; ++seq) {
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "-%02d.png", seq);
    candidate = dir / (pfx + "-" + tag + suffix);
  }
  if (stbi_write_png(candidate.string().c_str(), int(width), int(height), 4, pixels, int(width) * 4) == 0) {
    if (error) *error = "stbi_write_png failed for " + candidate.string();
    return false;
  }
  out_path = candidate;
  return true;
}

std::vector<PhotoGalleryEntry> list_photo_gallery(const std::filesystem::path& photos_dir) {
  std::vector<PhotoGalleryEntry> out;
  std::error_code ec;
  if (photos_dir.empty() || !std::filesystem::exists(photos_dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(photos_dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    auto ext = entry.path().extension().string();
    for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".png" && ext != ".bmp" && ext != ".jpg" && ext != ".jpeg") continue;
    PhotoGalleryEntry ge;
    ge.path = entry.path();
    ge.filename = entry.path().filename().string();
    ge.size_bytes = entry.file_size(ec);
    ge.thumb_key = "photo_gallery/thumb/" + ge.filename + "#" + std::to_string(ge.size_bytes);
    out.push_back(std::move(ge));
  }
  std::sort(out.begin(), out.end(), [](const PhotoGalleryEntry& a, const PhotoGalleryEntry& b) {
    return a.filename > b.filename;  // newest timestamp first
  });
  return out;
}

void PhotoModeController::set_text_resolver(std::function<std::string(const std::string&)> fn) {
  photo_text_fn() = std::move(fn);
}

void PhotoModeController::enter(const std::array<double, 3>& car_pos, double car_heading_rad,
                                const PhotoLimits& limits) {
  active_ = true;
  effects_open_ = true;
  ui_hidden_ = false;
  selected_row_ = kAperture;
  scroll_top_ = 0;
  flash_timer_ = 0.0;
  banner_timer_ = 0.0;
  accum_.cancel();
  camera_.reset(car_pos, car_heading_rad, limits);
  effects_.focus_distance = camera_.autofocus_distance(car_pos);
}

void PhotoModeController::exit() {
  active_ = false;
  gallery_open_ = false;
  accum_.cancel();
}

void PhotoModeController::open_gallery(const std::filesystem::path& photos_dir) {
  gallery_dir_ = photos_dir;
  gallery_entries_ = list_photo_gallery(photos_dir);
  gallery_selected_ = 0;
  gallery_preview_path_.clear();
  gallery_preview_valid_ = false;
  gallery_preview_w_ = 0;
  gallery_preview_h_ = 0;
  gallery_open_ = true;
  if (!gallery_entries_.empty()) {
    ensure_entry_thumbnail(0);
    ensure_selected_preview();
  }
}

void PhotoModeController::gallery_move(int delta) {
  if (gallery_entries_.empty()) return;
  const int n = int(gallery_entries_.size());
  gallery_selected_ = ((gallery_selected_ + delta) % n + n) % n;
  ensure_selected_preview();
}

void PhotoModeController::gallery_select(int idx) {
  if (gallery_entries_.empty()) return;
  gallery_selected_ = std::clamp(idx, 0, int(gallery_entries_.size()) - 1);
  ensure_selected_preview();
}

bool PhotoModeController::gallery_delete_selected() {
  if (gallery_entries_.empty() || gallery_selected_ < 0 || size_t(gallery_selected_) >= gallery_entries_.size())
    return false;
  std::error_code ec;
  const bool removed = std::filesystem::remove(gallery_entries_[size_t(gallery_selected_)].path, ec);
  if (removed) {
    gallery_entries_.erase(gallery_entries_.begin() + gallery_selected_);
    if (gallery_selected_ >= int(gallery_entries_.size()))
      gallery_selected_ = std::max(0, int(gallery_entries_.size()) - 1);
    gallery_preview_path_.clear();
    gallery_preview_valid_ = false;
    gallery_preview_w_ = 0;
    gallery_preview_h_ = 0;
    if (!gallery_entries_.empty()) ensure_selected_preview();
  }
  return removed;
}

bool PhotoModeController::ensure_entry_thumbnail(size_t idx) const {
  if (idx >= gallery_entries_.size()) return false;
  auto& ge = gallery_entries_[idx];
  if (ge.has_thumb && ui::atlas_find(ge.thumb_key, ge.thumb_uv)) return true;
  if (ge.thumb_tried && !ge.has_thumb) return false;
  ge.thumb_tried = true;
  std::vector<uint8_t> rgba;
  uint32_t tw = 0, th = 0, sw = 0, sh = 0;
  if (!load_photo_thumbnail(ge.path, 160u, 90u, rgba, tw, th, &sw, &sh)) {
    ge.has_thumb = false;
    return false;
  }
  ge.width = sw;
  ge.height = sh;
  ge.thumb_width = tw;
  ge.thumb_height = th;
  ge.has_thumb = ui::atlas_image(ge.thumb_key, tw, th, rgba, ge.thumb_uv, true);
  return ge.has_thumb;
}

bool PhotoModeController::ensure_selected_preview() const {
  if (gallery_entries_.empty() || gallery_selected_ < 0 || size_t(gallery_selected_) >= gallery_entries_.size()) {
    gallery_preview_valid_ = false;
    return false;
  }
  auto& ge = gallery_entries_[size_t(gallery_selected_)];
  if (gallery_preview_valid_ && gallery_preview_path_ == ge.path &&
      ui::atlas_find(gallery_preview_key_, gallery_preview_uv_)) {
    return true;
  }
  gallery_preview_path_ = ge.path;
  gallery_preview_key_ = "photo_gallery/preview/" + ge.filename + "#" + std::to_string(ge.size_bytes);
  std::vector<uint8_t> rgba;
  uint32_t pw = 0, ph = 0, sw = 0, sh = 0;
  if (!load_photo_thumbnail(ge.path, 512u, 288u, rgba, pw, ph, &sw, &sh)) {
    gallery_preview_valid_ = false;
    gallery_preview_w_ = 0;
    gallery_preview_h_ = 0;
    return false;
  }
  if (ge.width == 0) ge.width = sw;
  if (ge.height == 0) ge.height = sh;
  gallery_preview_w_ = pw;
  gallery_preview_h_ = ph;
  gallery_preview_valid_ = ui::atlas_image(gallery_preview_key_, pw, ph, rgba, gallery_preview_uv_, true);
  return gallery_preview_valid_;
}

int PhotoModeController::visible_effect_rows(float h) {
  const float s = std::max(0.25f, h / 720.0f);
  const float list_top = 66.0f * s;
  const float list_bot = h - 114.0f * s;
  const float row_pitch = 40.0f * s;
  return std::clamp(int(std::floor((list_bot - list_top) / row_pitch)), 1, int(kRowCount));
}

void PhotoModeController::select_row(int row) {
  if (row < 0 || row >= kRowCount) return;
  selected_row_ = row;
  const int vis = visible_effect_rows(720.0f);
  if (selected_row_ < scroll_top_) scroll_top_ = selected_row_;
  else if (selected_row_ >= scroll_top_ + vis) scroll_top_ = selected_row_ - vis + 1;
  scroll_top_ = std::clamp(scroll_top_, 0, std::max(0, int(kRowCount) - vis));
}

void PhotoModeController::move_row(int delta) {
  select_row(((selected_row_ + delta) % kRowCount + kRowCount) % kRowCount);
}

void PhotoModeController::adjust_row(int delta) {
  const float d = float(delta);
  switch (selected_row_) {
    case kPreset: {
      const int n = int(PhotoPreset::kCount);
      const int next = ((int(effects_.preset) + delta) % n + n) % n;
      effects_.apply_preset(static_cast<PhotoPreset>(next));
      break;
    }
    case kShutterSpeed: effects_.shutter_speed = std::clamp(effects_.shutter_speed + d * 5.0f, 0.0f, 100.0f); break;
    case kShutterMode: effects_.tracking_panning = !effects_.tracking_panning; break;
    case kAperture: effects_.aperture = std::clamp(effects_.aperture + d * 5.0f, 0.0f, 100.0f); break;
    case kFocusDistance: effects_.focus_distance = std::clamp(effects_.focus_distance + d * 0.5f, 1.5f, 120.0f); break;
    case kExposure: effects_.exposure = std::clamp(effects_.exposure + d * 4.0f, 0.0f, 100.0f); break;
    case kContrast: effects_.contrast = std::clamp(effects_.contrast + d * 4.0f, 0.0f, 100.0f); break;
    case kColour: effects_.colour = std::clamp(effects_.colour + d * 5.0f, 0.0f, 100.0f); break;
    case kBrightness: effects_.brightness = std::clamp(effects_.brightness + d * 4.0f, 0.0f, 100.0f); break;
    case kSepia: effects_.sepia = std::clamp(effects_.sepia + d * 5.0f, 0.0f, 100.0f); break;
    case kVignette: effects_.vignette = std::clamp(effects_.vignette + d * 5.0f, 0.0f, 100.0f); break;
    case kZoomFov: camera_.set_fov_deg(camera_.fov_deg() - double(delta) * 2.0); break;
    case kRollTilt: camera_.set_roll_deg(camera_.roll_deg() + double(delta) * 2.5); break;
    case kWatermark: effects_.watermark = !effects_.watermark; break;
    case kHideCar: effects_.hide_car = !effects_.hide_car; break;
    default: break;
  }
}

void PhotoModeController::autofocus(const std::array<double, 3>& target) {
  effects_.focus_distance = camera_.autofocus_distance(target);
}

void PhotoModeController::step(double dt, const PhotoCameraInput& input,
                               const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below,
                               const PhotoCamera::WallRaycastFn& wall_raycast) {
  if (!active_) return;
  time_ += dt;
  if (flash_timer_ > 0.0) flash_timer_ = std::max(0.0, flash_timer_ - dt);
  if (banner_timer_ > 0.0) banner_timer_ = std::max(0.0, banner_timer_ - dt);
  if (!accum_.active() && !gallery_open_) {
    camera_.step(dt, input, ground_below, wall_raycast);
  }
}

void PhotoModeController::begin_capture(uint32_t width, uint32_t height, int samples) {
  accum_.begin(width, height, samples);
}

void PhotoModeController::notify_photo_saved(const std::filesystem::path& saved_path, const PromoSpotResult& promo) {
  last_saved_label_ = saved_path.filename().string();
  last_promo_ = promo;
  flash_timer_ = 0.35;
  banner_timer_ = 5.5;
}

void PhotoModeController::build_hud(ui::List& out, float w, float h) const {
  if (!active_ || ui_hidden_) return;

  using ui::Align;
  using ui::Font;
  using ui::rgba;
  using ui::TextStyle;

  const float s = h / 720.0f;

  if (gallery_open_) {
    ui4::subtle_vignette(out, w, h, 0.36f);
    const std::string gal_title = std::string("PHOTO GALLERY");
    const std::string gal_eyebrow = std::string("HORIZON PHOTOS") + "  •  " +
                                    std::to_string(gallery_entries_.size()) + " SAVED";
    ui4::title_banner(out, s, 56.0f * s, 28.0f * s, gal_title, gal_eyebrow, true, 480.0f);

    // Authentic FH1 tilted UI4 gallery workspace (tilted cards + panel01 preview + floating pills).
    {
      ui4::TiltScope rot(out, 0.5f * w, 0.5f * h, ui4::kTiltRad * 0.65f);
      const float bx0 = 60.0f * s, bx1 = w - 60.0f * s;
      const float by0 = 126.0f * s, by1 = h - 88.0f * s;

      // Tilted brush header bar spanning the gallery workspace.
      ui4::brush(out, bx0 - 6.0f * s, by0, bx1 + 6.0f * s, by0 + 30.0f * s, ui4::kBrushBlack, true);
      out.text(bx0 + 14.0f * s, by0 + 8.0f * s, std::string("LOCAL PHOTOS FOLDER"),
               ui4::style(Font::kE, 13.0f * s, ui4::kOffWhite));
      if (!gallery_entries_.empty()) {
        char hdr_cnt[32];
        std::snprintf(hdr_cnt, sizeof hdr_cnt, "%d / %zu", gallery_selected_ + 1, gallery_entries_.size());
        out.text(bx1 - 14.0f * s, by0 + 8.0f * s, hdr_cnt,
                 ui4::style(Font::kE, 13.0f * s, ui4::kYellow, Align::kRight));
      }

      if (gallery_entries_.empty()) {
        ui4::panel01(out, bx0, by0 + 38.0f * s, bx1, by1, s, ui4::kCharcoalDark, ui4::kPink);
        out.text(0.5f * w, 0.5f * (by0 + 38.0f * s + by1) - 8.0f * s,
                 std::string("NO SAVED PHOTOS YET (PRESS ENTER / A IN PHOTO MODE TO SNAP)"),
                 ui4::style(Font::kB, 14.0f * s, ui4::kDim, Align::kCenter));
      } else {
        const float content_top = by0 + 38.0f * s;
        const float content_bot = by1 - 6.0f * s;
        const float inner_w = bx1 - bx0;
        const float list_x0 = bx0;
        const float list_x1 = list_x0 + inner_w * 0.43f;
        const float prev_x0 = list_x1 + 22.0f * s;
        const float prev_x1 = bx1;

        // Left pane: floating tilted UI4 PILL02/PILL03 list with real decoded thumbnails + scroll indicator.
        const float row_h = 58.0f * s;
        const int max_vis = std::max(1, int((content_bot - content_top) / row_h));
        const int total_g = int(gallery_entries_.size());
        const int start = std::clamp(gallery_selected_ - max_vis / 2, 0, std::max(0, total_g - max_vis));
        float last_ry1 = content_top;
        for (int i = start; i < total_g && i < start + max_vis; ++i) {
          ensure_entry_thumbnail(size_t(i));
          const auto& ge = gallery_entries_[size_t(i)];
          const float ry0 = content_top + float(i - start) * row_h;
          const float ry1 = ry0 + row_h - 6.0f * s;
          last_ry1 = ry1;
          const bool sel = (i == gallery_selected_);
          out.add_hit(list_x0, ry0, list_x1, ry1, ui::kHitRow, i);
          ui4::pill(out, list_x0, ry0, list_x1, ry1,
                    sel ? ui4::kPink : ((i % 2 == 0) ? ui4::kPillGrey : rgba(24, 26, 32, 232)),
                    (i % 2 == 0) ? ui4::PillKind::kPill02 : ui4::PillKind::kPill03, false, sel);

          // Decoded thumbnail slot (74x42 at 720p).
          const float tx0 = list_x0 + 8.0f * s;
          const float ty0 = ry0 + 5.0f * s;
          const float tw_box = 74.0f * s;
          const float th_box = (ry1 - ry0) - 10.0f * s;
          out.rect(tx0, ty0, tx0 + tw_box, ty0 + th_box, rgba(10, 12, 16, 235));
          if (ge.has_thumb) {
            out.image(tx0, ty0, tx0 + tw_box, ty0 + th_box, ge.thumb_uv);
          }
          out.frame(tx0, ty0, tx0 + tw_box, ty0 + th_box, sel ? ui4::kYellow : rgba(255, 255, 255, 52), 1.4f * s);

          const float text_x = tx0 + tw_box + 10.0f * s;
          const float text_max_w = std::max(40.0f * s, list_x1 - text_x - 10.0f * s);
          ui::TextStyle fts = ui4::style(sel ? Font::kE : Font::kB, 12.5f * s, ui4::kWhite);
          ui4::fit_text(ge.filename, fts, text_max_w, 8.5f * s);
          out.text(text_x, ry0 + 9.0f * s, ge.filename, fts);

          char met_buf[64];
          if (ge.width > 0 && ge.height > 0) {
            std::snprintf(met_buf, sizeof met_buf, "%ux%u  •  %.0f KB", ge.width, ge.height,
                          double(ge.size_bytes) / 1024.0);
          } else {
            std::snprintf(met_buf, sizeof met_buf, "%.0f KB", double(ge.size_bytes) / 1024.0);
          }
          out.text(text_x, ry0 + 29.0f * s, met_buf,
                   ui4::style(Font::kB, 10.5f * s, sel ? ui4::kYellow : ui4::kDim));
        }
        ui4::scroll_indicator(out, list_x1 + 6.0f * s, content_top, list_x1 + 13.0f * s,
                              std::max(content_top + 24.0f * s, last_ry1), start, max_vis, total_g, s);

        // Right pane: tilted FH1 PANEL01 preview card with pink top accent and metadata strip.
        ensure_selected_preview();
        const auto& sel_entry =
            gallery_entries_[size_t(std::clamp(gallery_selected_, 0, int(gallery_entries_.size()) - 1))];
        ui4::panel01(out, prev_x0, content_top, prev_x1, content_bot, s, ui4::kCharcoalDark, ui4::kPink);

        const float prev_footer_h = 56.0f * s;
        const float frame_x0 = prev_x0 + 10.0f * s;
        const float frame_x1 = prev_x1 - 10.0f * s;
        const float frame_y0 = content_top + 10.0f * s;
        const float frame_y1 = content_bot - prev_footer_h - 6.0f * s;
        out.rect(frame_x0, frame_y0, frame_x1, frame_y1, rgba(10, 12, 16, 240));
        out.frame(frame_x0, frame_y0, frame_x1, frame_y1, ui4::kPink, 1.8f * s);

        const float pad = 6.0f * s;
        const float avail_w = std::max(16.0f, (frame_x1 - frame_x0) - 2.0f * pad);
        const float avail_h = std::max(16.0f, (frame_y1 - frame_y0) - 2.0f * pad);
        if (gallery_preview_valid_ && gallery_preview_w_ > 0 && gallery_preview_h_ > 0) {
          const float img_aspect = float(gallery_preview_w_) / float(gallery_preview_h_);
          float dw = avail_w;
          float dh = dw / img_aspect;
          if (dh > avail_h) {
            dh = avail_h;
            dw = dh * img_aspect;
          }
          const float ix0 = 0.5f * (frame_x0 + frame_x1 - dw);
          const float iy0 = 0.5f * (frame_y0 + frame_y1 - dh);
          out.image(ix0, iy0, ix0 + dw, iy0 + dh, gallery_preview_uv_);
        } else {
          out.text(0.5f * (frame_x0 + frame_x1), 0.5f * (frame_y0 + frame_y1) - 8.0f * s,
                   std::string("PREVIEW UNAVAILABLE"),
                   ui4::style(Font::kB, 13.0f * s, ui4::kDim, Align::kCenter));
        }

        // Preview metadata strip inside the tilted card.
        const float fy0 = frame_y1 + 6.0f * s;
        const float fy1 = content_bot - 8.0f * s;
        ui4::pill(out, frame_x0, fy0, frame_x1, fy1, ui4::kPillGrey, ui4::PillKind::kHud01, false, true);
        ui::TextStyle pfn = ui4::style(Font::kE, 13.0f * s, ui4::kWhite);
        ui4::fit_text(sel_entry.filename, pfn, (frame_x1 - frame_x0) - 110.0f * s, 9.0f * s);
        out.text(frame_x0 + 12.0f * s, fy0 + 7.0f * s, sel_entry.filename, pfn);
        char idx_buf[32];
        std::snprintf(idx_buf, sizeof idx_buf, "%d / %zu", gallery_selected_ + 1, gallery_entries_.size());
        out.text(frame_x1 - 12.0f * s, fy0 + 7.0f * s, idx_buf,
                 ui4::style(Font::kE, 13.0f * s, ui4::kYellow, Align::kRight));
        char det_buf[96];
        std::snprintf(det_buf, sizeof det_buf, "%ux%u PX  •  %.1f KB", sel_entry.width, sel_entry.height,
                      double(sel_entry.size_bytes) / 1024.0);
        out.text(frame_x0 + 12.0f * s, fy0 + 26.0f * s, det_buf, ui4::style(Font::kB, 11.0f * s, ui4::kOffWhite));
      }
    }
    ui4::button_bar(out,
                    {{"X", loc_str("HelpButtons", "IDS_Delete", "DELETE PHOTO")},
                     {"B", loc_str("HelpButtons", "IDS_Back", "BACK TO CAMERA")}},
                    w, h, 56.0f);
    return;
  }
  const uint32_t kGrid = rgba(255, 255, 255, 30);
  const uint32_t kBracket = rgba(255, 255, 255, 165);

  // Subtle rule-of-thirds viewfinder grid + crisp corner framing brackets.
  const float mx = 42.0f * s, my = 26.0f * s;
  out.line(w / 3.0f, my, w / 3.0f, h - my, 1.0f * s, kGrid);
  out.line(2.0f * w / 3.0f, my, 2.0f * w / 3.0f, h - my, 1.0f * s, kGrid);
  out.line(mx, h / 3.0f, w - mx, h / 3.0f, 1.0f * s, kGrid);
  out.line(mx, 2.0f * h / 3.0f, w - mx, 2.0f * h / 3.0f, 1.0f * s, kGrid);

  const float bl = 22.0f * s;
  out.line(mx, my, mx + bl, my, 2.0f * s, kBracket);
  out.line(mx, my, mx, my + bl, 2.0f * s, kBracket);
  out.line(w - mx - bl, my, w - mx, my, 2.0f * s, kBracket);
  out.line(w - mx, my, w - mx, my + bl, 2.0f * s, kBracket);
  out.line(mx, h - my, mx + bl, h - my, 2.0f * s, kBracket);
  out.line(mx, h - my - bl, mx, h - my, 2.0f * s, kBracket);
  out.line(w - mx - bl, h - my, w - mx, h - my, 2.0f * s, kBracket);
  out.line(w - mx, h - my - bl, w - mx, h - my, 2.0f * s, kBracket);

  const float cx = 0.5f * w, cy = 0.5f * h;
  // Bind authentic 709_PHOTO_MODE focus reticle & shutter flash when disc scene is loaded.
  bool drew_photo_scene = false;
  {
    SceneScreen sc("709_PHOTO_MODE");
    if (sc.ok()) {
      const SceneFrame f = SceneScreen::frame(w, h);
      const SceneDraw& sd = sc.scene();
      sc.hide(sc.node("PHOTOMODE_TUTORIAL"));
      if (const int ret = sc.node("Photo_Reticle"); ret >= 0) {
        const int grp = sd.scene()->nodes[size_t(ret)].parent;
        if (grp >= 0) {
          sc.show(grp);
          sc.hide(sc.node("PHOTO_HELP", grp));
          sc.hide(sc.node("HOVER_PROGRESS", grp));
        }
        sc.show(ret, 0.24f);
      }
      if (const int fg = sc.node("Group_FLASH"); fg >= 0) {
        if (flash_timer_ > 0.0) {
          const float fa = float(std::clamp(flash_timer_ / 0.35, 0.0, 1.0)) * 0.55f;
          sc.show(fg, fa);
        } else {
          sc.hide(fg);
        }
      }
      sc.draw(out, f);
      drew_photo_scene = true;
    }
  }
  if (!drew_photo_scene) {
    std::array<float, 4> ret_uv{};
    if (ui4::texture("HUD/PHOTOMODEFOCUSRETICLE", ret_uv)) {
      const float rr = 64.0f * s;
      out.image(cx - rr, cy - rr, cx + rr, cy + rr, ret_uv, rgba(255, 255, 255, 52));
    } else {
      const float rr = 14.0f * s;
      out.line(cx - rr, cy, cx - 5.0f * s, cy, 1.5f * s, rgba(255, 255, 255, 80));
      out.line(cx + 5.0f * s, cy, cx + rr, cy, 1.5f * s, rgba(255, 255, 255, 80));
      out.line(cx, cy - rr, cx, cy - 5.0f * s, 1.5f * s, rgba(255, 255, 255, 80));
      out.line(cx, cy + 5.0f * s, cx, cy + rr, 1.5f * s, rgba(255, 255, 255, 80));
    }
  }

  // Compact top-left HUDPILL01 Horizon Promo + Camera Telemetry strip (NO top-center or top-right dark boxes!).
  {
    const float px0 = 56.0f * s;
    float py = 30.0f * s;
    const bool show_promo_banner =
        banner_timer_ > 0.0 && (!last_promo_.newly_spotted_ids.empty() || !last_saved_label_.empty());
    const PromoSpotResult& promo = show_promo_banner ? last_promo_ : live_promo_;

    // Row 1: Compact pink PHOTO MODE + camera readout pill (Metric m vs Imperial ft)
    const std::string pm_hdr = loc_str("PauseMenu", "IDS_Photo", "PHOTO MODE");
    const float focus_disp = metric_units_ ? effects_.focus_distance : (effects_.focus_distance * 3.28084f);
    const char* focus_unit = metric_units_ ? "m" : "ft";
    char cam_info[160];
    std::snprintf(cam_info, sizeof cam_info, "%s   •   FOV %.0f°   •   F/%.1f   •   %.1f%s%s",
                  pm_hdr.c_str(), camera_.fov_deg(), 1.4f + (1.0f - effects_.aperture / 100.0f) * 14.6f,
                  focus_disp, focus_unit, camera_.limits().through_walls ? "   •   NO-CLIP" : "");
    ui4::pill(out, px0, py, px0 + 382.0f * s, py + 26.0f * s, ui4::kPink, ui4::PillKind::kHud01);
    ui::TextStyle cis = ui4::style(Font::kE, 12.0f * s, ui4::kWhite);
    ui4::fit_text(cam_info, cis, 358.0f * s, 8.5f * s);
    out.text(px0 + 12.0f * s, py + 6.5f * s, cam_info, cis);
    py += 29.0f * s;

    // Row 2: Compact HUDPILL01 Horizon Promo strip
    char promo_line[160];
    uint32_t promo_col = ui4::kWhite;
    if (show_promo_banner && !last_promo_.newly_spotted_names.empty()) {
      std::snprintf(promo_line, sizeof promo_line, "PROMO %d/%d  •  NEW: %s (+%s Cr)", promo.total_spotted,
                    promo.total_cars, last_promo_.newly_spotted_names[0].c_str(),
                    format_credits(last_promo_.credits_awarded).c_str());
      promo_col = ui4::kYellow;
    } else if (show_promo_banner) {
      std::snprintf(promo_line, sizeof promo_line, "PROMO %d/%d  •  SAVED 4X SSAA PHOTO", promo.total_spotted,
                    promo.total_cars);
      promo_col = ui4::kGreen;
    } else if (!promo.visible_names.empty()) {
      const bool has_new = !promo.newly_spotted_ids.empty();
      std::snprintf(promo_line, sizeof promo_line, "PROMO %d/%d  •  %s%s", promo.total_spotted, promo.total_cars,
                    has_new ? "NEW: " : "IN FRAME: ", promo.visible_names[0].c_str());
      promo_col = has_new ? ui4::kYellow : ui4::kWhite;
    } else {
      std::snprintf(promo_line, sizeof promo_line, "HORIZON PROMO  %d / %d CARS SPOTTED", promo.total_spotted,
                    promo.total_cars);
      promo_col = ui4::kOffWhite;
    }
    ui4::pill(out, px0, py, px0 + 382.0f * s, py + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    ui::TextStyle prs = ui4::style(Font::kB, 11.5f * s, promo_col);
    ui4::fit_text(promo_line, prs, 358.0f * s, 8.5f * s);
    out.text(px0 + 12.0f * s, py + 5.5f * s, promo_line, prs);
  }

  // ---- 516_I_EFFECTSMODE: Sleek Right-Edge Scrollable Effects Column (x=1002..1252) ----
  if (effects_open_) {
    const float dx0 = w - 278.0f * s;  // canvas x = 1002 at 1280px (keeps x >= 1000 with scrollbar room!)
    const float dx1 = w - 28.0f * s;   // canvas x = 1252
    const float dy0 = 36.0f * s;
    const float row_pitch = 40.0f * s;
    const float row_h = 35.0f * s;
    const float list_top = dy0 + 30.0f * s;  // y = 66 at 720p
    const int vis = visible_effect_rows(h);
    if (selected_row_ < scroll_top_) scroll_top_ = selected_row_;
    else if (selected_row_ >= scroll_top_ + vis) scroll_top_ = selected_row_ - vis + 1;
    scroll_top_ = std::clamp(scroll_top_, 0, std::max(0, int(kRowCount) - vis));
    const int first = scroll_top_;
    const int last = std::min(int(kRowCount), first + vis);

    // Header PILL01 (#BFBFBF) + PILLCLIPON01 + compact position counter (e.g. "4 / 15")
    ui4::pill(out, dx0, dy0, dx1, dy0 + 26.0f * s, rgba(0xBF, 0xBF, 0xBF, 240), ui4::PillKind::kMenu, false, true);
    out.text(dx0 + 12.0f * s, dy0 + 6.0f * s, std::string("CAMERA EFFECTS"),
             ui4::style(Font::kD, 13.0f * s, ui4::kInk));
    char pos_buf[24];
    std::snprintf(pos_buf, sizeof pos_buf, "%d / %d", selected_row_ + 1, int(kRowCount));
    out.text(dx1 - 10.0f * s, dy0 + 6.5f * s, pos_buf,
             ui4::style(Font::kD, 11.5f * s, ui4::kPink, Align::kRight));

    struct RowInfo {
      std::string name;
      float norm;
      std::string value;
      bool is_bar;
    };
    auto fmt_pct = [](float v) {
      char b[16];
      std::snprintf(b, sizeof b, "%d", int(std::lround(v)));
      return std::string(b);
    };
    char fbuf[24], zbuf[24], rbuf[24];
    if (metric_units_) {
      std::snprintf(fbuf, sizeof fbuf, "%.1fm", effects_.focus_distance);
    } else {
      std::snprintf(fbuf, sizeof fbuf, "%.1fft", effects_.focus_distance * 3.28084f);
    }
    std::snprintf(zbuf, sizeof zbuf, "%.0f°", camera_.fov_deg());
    std::snprintf(rbuf, sizeof rbuf, "%+.0f°", camera_.roll_deg());

    const RowInfo rows[kRowCount] = {
        {std::string("PRESET"), 0.0f, photo_preset_name(effects_.preset), false},
        {loc_str("InGame", "IDS_Label_Shutterspeed", "SHUTTER"), effects_.shutter_speed / 100.0f, fmt_pct(effects_.shutter_speed), true},
        {std::string("BLUR MODE"), 0.0f, effects_.tracking_panning ? "TRACKING" : "PANNING", false},
        {loc_str("InGame", "IDS_Label_Aperture", "APERTURE"), effects_.aperture / 100.0f, fmt_pct(effects_.aperture), true},
        {loc_str("InGame", "IDS_Label_Focus", "FOCUS"), (effects_.focus_distance - 1.5f) / 118.5f, fbuf, true},
        {loc_str("InGame", "IDS_Label_Exposure", "EXPOSURE"), effects_.exposure / 100.0f, fmt_pct(effects_.exposure), true},
        {loc_str("InGame", "IDS_Label_Contrast", "CONTRAST"), effects_.contrast / 100.0f, fmt_pct(effects_.contrast), true},
        {loc_str("InGame", "IDS_Label_Color", "COLOUR"), effects_.colour / 100.0f, fmt_pct(effects_.colour), true},
        {loc_str("InGame", "IDS_Label_Brightness", "BRIGHTNESS"), effects_.brightness / 100.0f, fmt_pct(effects_.brightness), true},
        {loc_str("InGame", "IDS_Label_Sepia", "SEPIA"), effects_.sepia / 100.0f, fmt_pct(effects_.sepia), true},
        {loc_str("InGame", "IDS_Label_Vignette", "VIGNETTE"), effects_.vignette / 100.0f, fmt_pct(effects_.vignette), true},
        {std::string("ZOOM / FOV"),
         float((camera_.limits().max_fov_deg - camera_.fov_deg()) /
               std::max(1.0, camera_.limits().max_fov_deg - camera_.limits().min_fov_deg)),
         zbuf, true},
        {loc_str("InGame", "IDS_Photo_Mode_Roll", "ROLL / TILT"),
         float((camera_.roll_deg() + camera_.limits().max_roll_deg) / (2.0 * camera_.limits().max_roll_deg)), rbuf,
         true},
        {std::string("WATERMARK"), 0.0f, effects_.watermark ? "ON" : "OFF", false},
        {std::string("HIDE CAR"), 0.0f, effects_.hide_car ? "ON" : "OFF", false},
    };

    float list_drawn_bot = list_top;
    for (int i = first; i < last; ++i) {
      const float ry0 = list_top + float(i - first) * row_pitch;
      const float ry1 = ry0 + row_h;
      list_drawn_bot = ry1;
      const bool sel = (i == selected_row_);

      // Floating PILL01 row (#212121, focused #F90064) — never collides with bottom button bar!
      out.add_hit(dx0, ry0, dx1, ry1, ui::kHitRow, i);
      ui4::pill(out, dx0, ry0, dx1, ry1, sel ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kMenu, false, sel);

      out.text(dx0 + 12.0f * s, ry0 + 4.5f * s, rows[i].name,
               ui4::style(sel ? Font::kD : Font::kB, 11.0f * s, sel ? ui4::kWhite : ui4::kOffWhite));
      if (!rows[i].is_bar) {
        out.add_hit(dx0 + 96.0f * s, ry0, dx0 + 132.0f * s, ry1, ui::kHitStepLeft, i);
        out.add_hit(dx1 - 32.0f * s, ry0, dx1, ry1, ui::kHitStepRight, i);
        ui4::opt_arrow(out, dx0 + 112.0f * s, ry0 + 22.0f * s, 11.0f * s, -1, sel ? ui4::kWhite : ui4::kDim);
        ui4::opt_arrow(out, dx1 - 14.0f * s, ry0 + 22.0f * s, 11.0f * s, +1, sel ? ui4::kWhite : ui4::kDim);
        ui::TextStyle vs = ui4::style(Font::kD, 11.5f * s, sel ? ui4::kYellow : ui4::kWhite, Align::kCenter);
        ui4::fit_text(rows[i].value, vs, (dx1 - dx0) - 134.0f * s, 8.0f * s);
        out.text(0.5f * (dx0 + 112.0f * s + dx1 - 14.0f * s), ry0 + 16.5f * s, rows[i].value, vs);
      } else {
        const float sl_x0 = dx0 + 12.0f * s, sl_x1 = dx1 - 12.0f * s;
        const float sl_mid = 0.5f * (sl_x0 + sl_x1);
        out.add_hit(sl_x0, ry1 - 15.0f * s, sl_mid, ry1, ui::kHitStepLeft, i);
        out.add_hit(sl_mid, ry1 - 15.0f * s, sl_x1, ry1, ui::kHitStepRight, i);
        out.text(dx1 - 12.0f * s, ry0 + 4.5f * s, rows[i].value,
                 ui4::style(Font::kD, 11.5f * s, sel ? ui4::kYellow : ui4::kWhite, Align::kRight));
        ui4::opt_slider(out, sl_x0, ry1 - 11.0f * s, sl_x1, ry1 - 4.0f * s, rows[i].norm, s,
                        sel ? ui4::kYellow : ui4::kPink, sel);
      }
    }

    // Visible vertical scrollbar indicator alongside the 15-slider effects drawer.
    if (vis < int(kRowCount)) {
      ui4::scroll_indicator(out, dx1 + 5.0f * s, list_top, dx1 + 12.0f * s, list_drawn_bot, first, vis, int(kRowCount),
                            s);
    }
  }

  // Supersampling capture progress indicator.
  if (accum_.active()) {
    const float bx0 = cx - 150.0f * s, bx1 = cx + 150.0f * s, by0 = cy + 36.0f * s, by1 = cy + 72.0f * s;
    ui4::brush(out, bx0, by0, bx1, by1, ui4::kBrushBlack);
    char msg[64];
    std::snprintf(msg, sizeof msg, "CAPTURING %d / %d SSAA...", accum_.sample_index() + 1, accum_.target_samples());
    out.text(cx, by0 + 11.0f * s, msg, ui4::style(Font::kE, 13.0f * s, ui4::kYellow, Align::kCenter));
  }

  // Shutter flash feedback (fallback when 709_PHOTO_MODE scene is not loaded).
  if (!drew_photo_scene && flash_timer_ > 0.0) {
    const float alpha = float(std::clamp(flash_timer_ / 0.35, 0.0, 1.0)) * 0.55f;
    out.rect(0, 0, w, h, rgba(255, 255, 255, uint8_t(alpha * 255.0f)));
  }

  // Authentic FH1 BUTTON_BAR_SCENE bottom bar (localized via string tables with English fallbacks).
  ui4::button_bar(out,
                  {{"A", loc_str("HelpButtons", "IDS_TakePhoto", "SNAP PHOTO")},
                   {"Y", loc_str("HelpButtons", "IDS_PhotoMode_Effects", "EFFECTS")},
                   {"DPAD_LR", "ADJUST"},
                   {"X", std::string("AUTOFOCUS")},
                   {"LB", std::string("RESET")},
                   {"B", loc_str("HelpButtons", "IDS_Back", "EXIT")}},
                  w, h, 56.0f);
}

}  // namespace carview
