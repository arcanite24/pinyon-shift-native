#include "photo_mode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <stb_image_write.h>

#include "ui4_chrome.h"

namespace carview {
namespace {

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

void blend_px(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int x, int y, uint8_t r, uint8_t g, uint8_t b,
              uint8_t a) {
  if (x < 0 || y < 0 || x >= int(w) || y >= int(h) || a == 0) return;
  const size_t i = (size_t(y) * w + size_t(x)) * 4;
  const uint32_t alpha = a;
  const uint32_t inv = 255u - alpha;
  rgba[i + 0] = uint8_t((rgba[i + 0] * inv + r * alpha) / 255u);
  rgba[i + 1] = uint8_t((rgba[i + 1] * inv + g * alpha) / 255u);
  rgba[i + 2] = uint8_t((rgba[i + 2] * inv + b * alpha) / 255u);
  rgba[i + 3] = 255;
}

uint32_t wm_glyph_rows(char ch) {
  // Compact 5x5 bitmap rows (5 bits per row, MSB left, 5 rows packed in 25 bits).
  switch (ch) {
    case 'A': return (0x0Eu << 20) | (0x11u << 15) | (0x1Fu << 10) | (0x11u << 5) | 0x11u;
    case 'E': return (0x1Fu << 20) | (0x10u << 15) | (0x1Eu << 10) | (0x10u << 5) | 0x1Fu;
    case 'F': return (0x1Fu << 20) | (0x10u << 15) | (0x1Eu << 10) | (0x10u << 5) | 0x10u;
    case 'G': return (0x0Eu << 20) | (0x10u << 15) | (0x17u << 10) | (0x11u << 5) | 0x0Eu;
    case 'H': return (0x11u << 20) | (0x11u << 15) | (0x1Fu << 10) | (0x11u << 5) | 0x11u;
    case 'I': return (0x0Eu << 20) | (0x04u << 15) | (0x04u << 10) | (0x04u << 5) | 0x0Eu;
    case 'N': return (0x11u << 20) | (0x19u << 15) | (0x15u << 10) | (0x13u << 5) | 0x11u;
    case 'O': return (0x0Eu << 20) | (0x11u << 15) | (0x11u << 10) | (0x11u << 5) | 0x0Eu;
    case 'R': return (0x1Eu << 20) | (0x11u << 15) | (0x1Eu << 10) | (0x14u << 5) | 0x13u;
    case 'S': return (0x0Fu << 20) | (0x10u << 15) | (0x0Eu << 10) | (0x01u << 5) | 0x1Eu;
    case 'T': return (0x1Fu << 20) | (0x04u << 15) | (0x04u << 10) | (0x04u << 5) | 0x04u;
    case 'Z': return (0x1Fu << 20) | (0x02u << 15) | (0x04u << 10) | (0x08u << 5) | 0x1Fu;
    case '/': return (0x01u << 20) | (0x02u << 15) | (0x04u << 10) | (0x08u << 5) | 0x10u;
    default: return 0u;
  }
}

void draw_wm_text(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int x0, int y0, int scale,
                  const std::string& text, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  int cx = x0;
  for (char ch : text) {
    const uint32_t bits = wm_glyph_rows(ch);
    for (int row = 0; row < 5; ++row) {
      const uint32_t rb = (bits >> (20 - row * 5)) & 0x1Fu;
      for (int col = 0; col < 5; ++col) {
        if ((rb & (0x10u >> col)) == 0) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx)
            blend_px(rgba, w, h, cx + col * scale + sx, y0 + row * scale + sy, r, g, b, a);
      }
    }
    cx += 6 * scale;
  }
}

}  // namespace

void stamp_photo_watermark(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  if (width < 120 || height < 60 || rgba.size() < size_t(width) * height * 4) return;
  const int scale = std::clamp(int(height / 360u), 1, 4);
  const std::string text = "FORZA HORIZON  //  SHIFT ENGINE";
  const int tw = int(text.size()) * 6 * scale;
  const int th = 5 * scale;
  const int pad_x = 8 * scale;
  const int pad_y = 5 * scale;
  const int bw = tw + pad_x * 2 + 4 * scale;
  const int bh = th + pad_y * 2;
  const int x0 = int(width) - bw - 14 * scale;
  const int y0 = int(height) - bh - 12 * scale;
  if (x0 < 0 || y0 < 0) return;

  // Translucent charcoal pill + Horizon pink accent stripe on left edge.
  for (int y = y0; y < y0 + bh; ++y) {
    for (int x = x0; x < x0 + bw; ++x) {
      const bool accent = (x < x0 + 4 * scale);
      blend_px(rgba, width, height, x, y, accent ? 245 : 14, accent ? 0 : 14, accent ? 96 : 18, accent ? 230 : 148);
    }
  }
  draw_wm_text(rgba, width, height, x0 + 4 * scale + pad_x + scale, y0 + pad_y + scale, scale, text, 0, 0, 0, 140);
  draw_wm_text(rgba, width, height, x0 + 4 * scale + pad_x, y0 + pad_y, scale, text, 255, 255, 255, 232);
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
    if (ext != ".png") continue;
    PhotoGalleryEntry ge;
    ge.path = entry.path();
    ge.filename = entry.path().filename().string();
    ge.size_bytes = entry.file_size(ec);
    out.push_back(std::move(ge));
  }
  std::sort(out.begin(), out.end(), [](const PhotoGalleryEntry& a, const PhotoGalleryEntry& b) {
    return a.filename > b.filename;  // newest timestamp first
  });
  return out;
}

void PhotoModeController::enter(const std::array<double, 3>& car_pos, double car_heading_rad,
                                const PhotoLimits& limits) {
  active_ = true;
  effects_open_ = true;
  ui_hidden_ = false;
  selected_row_ = kAperture;
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
  gallery_open_ = true;
}

void PhotoModeController::gallery_move(int delta) {
  if (gallery_entries_.empty()) return;
  const int n = int(gallery_entries_.size());
  gallery_selected_ = ((gallery_selected_ + delta) % n + n) % n;
}

void PhotoModeController::gallery_select(int idx) {
  if (gallery_entries_.empty()) return;
  gallery_selected_ = std::clamp(idx, 0, int(gallery_entries_.size()) - 1);
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
  }
  return removed;
}

void PhotoModeController::move_row(int delta) {
  selected_row_ = ((selected_row_ + delta) % kRowCount + kRowCount) % kRowCount;
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
    ui4::subtle_vignette(out, w, h, 0.40f);
    ui4::title_banner(out, s, 56.0f * s, 28.0f * s, "PHOTO GALLERY",
                      std::to_string(gallery_entries_.size()) + " SAVED PHOTOS", true, 460.0f);
    const float bx0 = 56.0f * s, bx1 = w - 56.0f * s;
    const float by0 = 122.0f * s, by1 = h - 84.0f * s;
    ui4::panel(out, bx0, by0, bx1, by1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    ui4::offwhite_strip(out, bx0 + 14.0f * s, by0 + 10.0f * s, bx1 - 14.0f * s, by0 + 34.0f * s, s, false);
    out.text(bx0 + 24.0f * s, by0 + 15.0f * s, "LOCAL PHOTOS FOLDER", ui4::style(Font::kE, 13.0f * s, ui4::kInk));
    if (gallery_entries_.empty()) {
      out.text(0.5f * w, 0.5f * h - 10.0f * s, "NO SAVED PHOTOS YET (PRESS ENTER / A IN PHOTO MODE TO SNAP)",
               ui4::style(Font::kB, 14.0f * s, ui4::kDim, Align::kCenter));
    } else {
      const float row_h = 44.0f * s;
      const int max_vis = std::max(1, int((by1 - by0 - 54.0f * s) / row_h));
      const int start = std::clamp(gallery_selected_ - max_vis / 2, 0,
                                   std::max(0, int(gallery_entries_.size()) - max_vis));
      for (int i = start; i < int(gallery_entries_.size()) && i < start + max_vis; ++i) {
        const auto& ge = gallery_entries_[size_t(i)];
        const float ry0 = by0 + 44.0f * s + float(i - start) * row_h;
        const float ry1 = ry0 + row_h - 5.0f * s;
        const bool sel = (i == gallery_selected_);
        out.add_hit(bx0 + 10.0f * s, ry0, bx1 - 10.0f * s, ry1, ui::kHitRow, i);
        ui4::pill(out, bx0 + 10.0f * s, ry0, bx1 - 10.0f * s, ry1,
                  sel ? ui4::kPink : ((i % 2 == 0) ? ui4::kPillGrey : rgba(24, 26, 32, 232)),
                  ui4::PillKind::kPill02, false, sel);
        out.text(bx0 + 26.0f * s, ry0 + 11.0f * s, ge.filename,
                 ui4::style(sel ? Font::kE : Font::kB, 13.5f * s, ui4::kWhite));
        char szbuf[32];
        std::snprintf(szbuf, sizeof szbuf, "%.0f KB", double(ge.size_bytes) / 1024.0);
        out.text(bx1 - 26.0f * s, ry0 + 11.0f * s, szbuf,
                 ui4::style(Font::kB, 12.0f * s, sel ? ui4::kYellow : ui4::kDim, Align::kRight));
      }
    }
    ui4::button_bar(out, {{"X", "DELETE PHOTO"}, {"B", "BACK TO CAMERA"}}, w, h, 56.0f);
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

  // 516_I_EFFECTSMODE FOCUS_RETICLE [576,296..704,424] subtle alpha=0.20 (NEVER obscures the car!)
  const float cx = 0.5f * w, cy = 0.5f * h;
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

  // Compact top-left HUDPILL01 Horizon Promo + Camera Telemetry strip (NO top-center or top-right dark boxes!).
  {
    const float px0 = 56.0f * s;
    float py = 30.0f * s;
    const bool show_promo_banner =
        banner_timer_ > 0.0 && (!last_promo_.newly_spotted_ids.empty() || !last_saved_label_.empty());
    const PromoSpotResult& promo = show_promo_banner ? last_promo_ : live_promo_;

    // Row 1: Compact pink PHOTO MODE + camera readout pill
    char cam_info[128];
    std::snprintf(cam_info, sizeof cam_info, "PHOTO MODE   •   FOV %.0f°   •   F/%.1f   •   %.1fm%s",
                  camera_.fov_deg(), 1.4f + (1.0f - effects_.aperture / 100.0f) * 14.6f, effects_.focus_distance,
                  camera_.limits().through_walls ? "   •   NO-CLIP" : "");
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

  // ---- 516_I_EFFECTSMODE: Sleek Right-Edge Effects Column (x=1012..1260) using PILL01 + OPT_SLIDER_FRAME ----
  if (effects_open_) {
    const float dx0 = w - 268.0f * s;  // canvas x = 1012 at 1280px!
    const float dx1 = w - 20.0f * s;   // canvas x = 1260
    const float dy0 = 36.0f * s;
    const float row_pitch = 38.0f * s;

    // Header PILL01 (#BFBFBF) + PILLCLIPON01
    ui4::pill(out, dx0, dy0, dx1, dy0 + 26.0f * s, rgba(0xBF, 0xBF, 0xBF, 240), ui4::PillKind::kMenu, false, true);
    out.text(dx0 + 12.0f * s, dy0 + 6.0f * s, "CAMERA EFFECTS", ui4::style(Font::kD, 13.0f * s, ui4::kInk));

    struct RowInfo {
      const char* name;
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
    std::snprintf(fbuf, sizeof fbuf, "%.1fm", effects_.focus_distance);
    std::snprintf(zbuf, sizeof zbuf, "%.0f°", camera_.fov_deg());
    std::snprintf(rbuf, sizeof rbuf, "%+.0f°", camera_.roll_deg());

    const RowInfo rows[kRowCount] = {
        {"PRESET", 0.0f, photo_preset_name(effects_.preset), false},
        {"SHUTTER", effects_.shutter_speed / 100.0f, fmt_pct(effects_.shutter_speed), true},
        {"BLUR MODE", 0.0f, effects_.tracking_panning ? "TRACKING" : "PANNING", false},
        {"APERTURE", effects_.aperture / 100.0f, fmt_pct(effects_.aperture), true},
        {"FOCUS", (effects_.focus_distance - 1.5f) / 118.5f, fbuf, true},
        {"EXPOSURE", effects_.exposure / 100.0f, fmt_pct(effects_.exposure), true},
        {"CONTRAST", effects_.contrast / 100.0f, fmt_pct(effects_.contrast), true},
        {"COLOUR", effects_.colour / 100.0f, fmt_pct(effects_.colour), true},
        {"BRIGHTNESS", effects_.brightness / 100.0f, fmt_pct(effects_.brightness), true},
        {"SEPIA", effects_.sepia / 100.0f, fmt_pct(effects_.sepia), true},
        {"VIGNETTE", effects_.vignette / 100.0f, fmt_pct(effects_.vignette), true},
        {"ZOOM / FOV",
         float((camera_.limits().max_fov_deg - camera_.fov_deg()) /
               std::max(1.0, camera_.limits().max_fov_deg - camera_.limits().min_fov_deg)),
         zbuf, true},
        {"ROLL / TILT",
         float((camera_.roll_deg() + camera_.limits().max_roll_deg) / (2.0 * camera_.limits().max_roll_deg)), rbuf,
         true},
        {"WATERMARK", 0.0f, effects_.watermark ? "ON" : "OFF", false},
        {"HIDE CAR", 0.0f, effects_.hide_car ? "ON" : "OFF", false},
    };

    const float list_top = dy0 + 30.0f * s;
    for (int i = 0; i < kRowCount; ++i) {
      const float ry0 = list_top + float(i) * row_pitch;
      const float ry1 = ry0 + 34.0f * s;
      const bool sel = (i == selected_row_);

      // Floating PILL01 row (#212121, focused #F90064) — NO giant enclosing dark wall!
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
  }

  // Supersampling capture progress indicator.
  if (accum_.active()) {
    const float bx0 = cx - 150.0f * s, bx1 = cx + 150.0f * s, by0 = cy + 36.0f * s, by1 = cy + 72.0f * s;
    ui4::brush(out, bx0, by0, bx1, by1, ui4::kBrushBlack);
    char msg[64];
    std::snprintf(msg, sizeof msg, "CAPTURING %d / %d SSAA...", accum_.sample_index() + 1, accum_.target_samples());
    out.text(cx, by0 + 11.0f * s, msg, ui4::style(Font::kE, 13.0f * s, ui4::kYellow, Align::kCenter));
  }

  // Shutter flash feedback.
  if (flash_timer_ > 0.0) {
    const float alpha = float(std::clamp(flash_timer_ / 0.35, 0.0, 1.0)) * 0.55f;
    out.rect(0, 0, w, h, rgba(255, 255, 255, uint8_t(alpha * 255.0f)));
  }

  // Authentic FH1 BUTTON_BAR_SCENE bottom bar.
  ui4::button_bar(out,
                  {{"A", "SNAP PHOTO"},
                   {"Y", "EFFECTS"},
                   {"DPAD_LR", "ADJUST"},
                   {"X", "AUTOFOCUS"},
                   {"LB", "RESET"},
                   {"B", "EXIT"}},
                  w, h, 56.0f);
}

}  // namespace carview
