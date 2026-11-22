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
  if (out.auto_exposure) {
    out.exposure += ev_offset;
  } else {
    out.exposure *= std::exp2(ev_offset);
  }
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
  out.motion_blur_strength = sh * 0.85f;
  out.motion_blur_car_protect = std::clamp(focus_distance + 1.5f, 4.0f, 25.0f);
  return out;
}

float PhotoEffects::wheel_blur_omega_boost() const {
  return std::clamp(shutter_speed / 100.0f, 0.0f, 1.0f) * 65.0f;
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
    const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below) {
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
}

void PhotoCamera::step(double dt, const PhotoCameraInput& in,
                       const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below) {
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

  clamp_to_limits(ground_below);
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

bool write_photo_png(const std::filesystem::path& dir, const std::vector<uint8_t>& rgba, uint32_t width,
                     uint32_t height, const std::string& prefix, std::filesystem::path& out_path, std::string* error) {
  if (width == 0 || height == 0 || rgba.size() < size_t(width) * height * 4) {
    if (error) *error = "invalid image buffer";
    return false;
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
  if (stbi_write_png(candidate.string().c_str(), int(width), int(height), 4, rgba.data(), int(width) * 4) == 0) {
    if (error) *error = "stbi_write_png failed for " + candidate.string();
    return false;
  }
  out_path = candidate;
  return true;
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
  accum_.cancel();
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
    default: break;
  }
}

void PhotoModeController::autofocus(const std::array<double, 3>& target) {
  effects_.focus_distance = camera_.autofocus_distance(target);
}

void PhotoModeController::step(double dt, const PhotoCameraInput& input,
                               const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below) {
  if (!active_) return;
  time_ += dt;
  if (flash_timer_ > 0.0) flash_timer_ = std::max(0.0, flash_timer_ - dt);
  if (banner_timer_ > 0.0) banner_timer_ = std::max(0.0, banner_timer_ - dt);
  if (!accum_.active()) {
    camera_.step(dt, input, ground_below);
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
  const uint32_t kPink = rgba(0xF5, 0x04, 0x5C);
  const uint32_t kAmber = rgba(255, 190, 48);
  const uint32_t kGlass = rgba(10, 12, 18, 206);
  const uint32_t kGlassHi = rgba(24, 28, 38, 224);
  const uint32_t kWhite = rgba(246, 248, 252);
  const uint32_t kDim = rgba(170, 178, 192);
  const uint32_t kGrid = rgba(255, 255, 255, 34);
  const uint32_t kBracket = rgba(255, 255, 255, 150);

  // Subtle rule-of-thirds viewfinder grid + corner framing brackets.
  const float mx = 42.0f * s, my = 34.0f * s;
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

  // Centre autofocus reticle.
  const float cx = 0.5f * w, cy = 0.5f * h;
  const float rr = 14.0f * s;
  out.line(cx - rr, cy, cx - 5.0f * s, cy, 1.5f * s, kBracket);
  out.line(cx + 5.0f * s, cy, cx + rr, cy, 1.5f * s, kBracket);
  out.line(cx, cy - rr, cx, cy - 5.0f * s, 1.5f * s, kBracket);
  out.line(cx, cy + 5.0f * s, cx, cy + rr, 1.5f * s, kBracket);

  // Top-left header strip: PHOTO MODE + camera limits badges.
  out.rect(mx + 6 * s, my + 6 * s, mx + 290 * s, my + 42 * s, kGlass, 4 * s);
  out.rect(mx + 6 * s, my + 6 * s, mx + 12 * s, my + 42 * s, kPink, 2 * s);
  out.text(mx + 22 * s, my + 14 * s, "PHOTO MODE",
           TextStyle{.size = 18 * s, .weight = 0.12f, .tracking = 0.14f, .color = kWhite, .font = Font::kE});

  char cam_info[128];
  std::snprintf(cam_info, sizeof cam_info, "FOV %.0f   F/%.1f   %.1fm%s%s", camera_.fov_deg(),
                1.4f + (1.0f - effects_.aperture / 100.0f) * 14.6f, effects_.focus_distance,
                camera_.limits().cylinder_radius > 150.0 ? "   WIDER LIMITS" : "",
                camera_.limits().through_walls ? "   NO-CLIP" : "");
  out.rect(mx + 300 * s, my + 6 * s, mx + 720 * s, my + 42 * s, kGlass, 4 * s);
  out.text(mx + 314 * s, my + 17 * s, cam_info,
           TextStyle{.size = 11.5f * s, .weight = 0.11f, .tracking = 0.08f, .color = kDim, .font = Font::kB});

  // Top-right Horizon Promo badge / live detection card.
  const bool show_promo_banner = banner_timer_ > 0.0 && (!last_promo_.newly_spotted_ids.empty() || !last_saved_label_.empty());
  const PromoSpotResult& promo = show_promo_banner ? last_promo_ : live_promo_;
  const float px1 = w - mx - 6 * s;
  const float px0 = px1 - 320 * s;
  const float py0 = my + 6 * s;
  const float py1 = py0 + (show_promo_banner ? 116 * s : 74 * s);
  out.shadow(px0, py0 + 3 * s, px1, py1 + 3 * s, rgba(0, 0, 0, 120), 6 * s, 10 * s);
  out.rect(px0, py0, px1, py1, kGlass, 6 * s, kGlassHi);
  out.rect(px0, py0, px1, py0 + 4 * s, show_promo_banner && !last_promo_.newly_spotted_ids.empty() ? kAmber : kPink);

  out.text(px0 + 14 * s, py0 + 13 * s, "HORIZON PROMO",
           TextStyle{.size = 11 * s, .weight = 0.12f, .tracking = 0.18f, .color = kAmber, .font = Font::kE});
  char cnt_buf[48];
  std::snprintf(cnt_buf, sizeof cnt_buf, "%d / %d CARS", promo.total_spotted, promo.total_cars);
  out.text(px1 - 14 * s, py0 + 13 * s, cnt_buf,
           TextStyle{.size = 11 * s, .weight = 0.11f, .color = kWhite, .align = Align::kRight, .font = Font::kB});

  if (show_promo_banner) {
    if (!last_promo_.newly_spotted_names.empty()) {
      std::string cars_line = "NEW: " + last_promo_.newly_spotted_names[0];
      if (last_promo_.newly_spotted_names.size() > 1)
        cars_line += " +" + std::to_string(last_promo_.newly_spotted_names.size() - 1) + " MORE";
      out.text(px0 + 14 * s, py0 + 36 * s, cars_line,
               TextStyle{.size = 12 * s, .weight = 0.11f, .color = kWhite, .font = Font::kB});
      out.text(px0 + 14 * s, py0 + 58 * s,
               "+" + format_credits(last_promo_.credits_awarded) + " CR" +
                   (last_promo_.milestone_reached ? "  (MILESTONE BONUS!)" : ""),
               TextStyle{.size = 13 * s, .weight = 0.12f, .color = kAmber, .font = Font::kE});
    } else {
      out.text(px0 + 14 * s, py0 + 40 * s, "PHOTO CAPTURED (4X SSAA)",
               TextStyle{.size = 12 * s, .weight = 0.12f, .color = kWhite, .font = Font::kE});
    }
    if (!last_saved_label_.empty()) {
      out.text(px0 + 14 * s, py0 + 84 * s, "SAVED: " + last_saved_label_,
               TextStyle{.size = 10 * s, .weight = 0.10f, .color = kDim, .font = Font::kB});
    }
  } else {
    if (!promo.visible_names.empty()) {
      const bool has_new = !promo.newly_spotted_ids.empty();
      std::string line = (has_new ? "NEW IN FRAME: " : "IN FRAME: ") + promo.visible_names[0];
      if (promo.visible_names.size() > 1)
        line += " (+" + std::to_string(promo.visible_names.size() - 1) + ")";
      out.text(px0 + 14 * s, py0 + 36 * s, line,
               TextStyle{.size = 11.5f * s, .weight = 0.11f, .color = has_new ? kAmber : kWhite, .font = Font::kB});
      out.text(px0 + 14 * s, py0 + 54 * s,
               has_new ? "SNAP TO EARN +" + format_credits(promo.credits_awarded) + " CR" : "ALREADY PHOTOGRAPHED",
               TextStyle{.size = 9.5f * s, .weight = 0.10f, .color = kDim, .font = Font::kB});
    } else {
      out.text(px0 + 14 * s, py0 + 40 * s, "FRAME A CAR TO SPOT FOR PROMO",
               TextStyle{.size = 10.5f * s, .weight = 0.10f, .color = kDim, .font = Font::kB});
    }
  }

  // Left collapsible Photo Effects slider drawer.
  if (effects_open_) {
    const float dx0 = mx + 6 * s;
    const float dx1 = dx0 + 318 * s;
    const float dy0 = my + 56 * s;
    const float row_h = 34.0f * s;
    const float dy1 = dy0 + 34 * s + float(kRowCount) * row_h + 12 * s;
    out.shadow(dx0, dy0 + 4 * s, dx1, dy1 + 4 * s, rgba(0, 0, 0, 120), 6 * s, 10 * s);
    out.rect(dx0, dy0, dx1, dy1, kGlass, 6 * s, kGlassHi);
    out.text(dx0 + 14 * s, dy0 + 10 * s, "CAMERA EFFECTS",
             TextStyle{.size = 11 * s, .weight = 0.12f, .tracking = 0.16f, .color = kPink, .font = Font::kE});

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
    std::snprintf(zbuf, sizeof zbuf, "%.0f DEG", camera_.fov_deg());
    std::snprintf(rbuf, sizeof rbuf, "%+.0f DEG", camera_.roll_deg());

    const RowInfo rows[kRowCount] = {
        {"PRESET", 0.0f, photo_preset_name(effects_.preset), false},
        {"SHUTTER SPEED", effects_.shutter_speed / 100.0f, fmt_pct(effects_.shutter_speed), true},
        {"APERTURE (DOF)", effects_.aperture / 100.0f, fmt_pct(effects_.aperture), true},
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
        {"ROLL / TILT", float((camera_.roll_deg() + camera_.limits().max_roll_deg) / (2.0 * camera_.limits().max_roll_deg)),
         rbuf, true},
    };

    for (int i = 0; i < kRowCount; ++i) {
      const float ry0 = dy0 + 34 * s + float(i) * row_h;
      const float ry1 = ry0 + row_h - 3 * s;
      const bool sel = (i == selected_row_);
      if (sel) {
        out.rect(dx0 + 6 * s, ry0, dx1 - 6 * s, ry1, rgba(0xF5, 0x04, 0x5C, 210), 4 * s);
      }
      out.text(dx0 + 16 * s, ry0 + 6 * s, rows[i].name,
               TextStyle{.size = 10.5f * s, .weight = 0.11f, .color = sel ? kWhite : kDim, .font = Font::kB});
      out.text(dx1 - 16 * s, ry0 + 6 * s, rows[i].value,
               TextStyle{.size = 10.5f * s, .weight = 0.11f, .color = kWhite, .align = Align::kRight, .font = Font::kB});
      if (rows[i].is_bar) {
        const float bx0 = dx0 + 16 * s, bx1 = dx1 - 16 * s, by0 = ry1 - 7 * s, by1 = ry1 - 3 * s;
        out.rect(bx0, by0, bx1, by1, rgba(255, 255, 255, sel ? 85 : 42), 2 * s);
        const float fill_x = bx0 + (bx1 - bx0) * std::clamp(rows[i].norm, 0.0f, 1.0f);
        out.rect(bx0, by0, std::max(bx0 + 3 * s, fill_x), by1, sel ? kWhite : kAmber, 2 * s);
      }
    }
  }

  // Supersampling capture progress indicator.
  if (accum_.active()) {
    const float bx0 = cx - 140 * s, bx1 = cx + 140 * s, by0 = cy + 32 * s, by1 = cy + 68 * s;
    out.rect(bx0, by0, bx1, by1, kGlassHi, 6 * s);
    char msg[64];
    std::snprintf(msg, sizeof msg, "CAPTURING %d / %d SSAA...", accum_.sample_index() + 1, accum_.target_samples());
    out.text(cx, by0 + 11 * s, msg,
             TextStyle{.size = 12 * s, .weight = 0.12f, .color = kAmber, .align = Align::kCenter, .font = Font::kE});
  }

  // Shutter flash feedback.
  if (flash_timer_ > 0.0) {
    const float alpha = float(std::clamp(flash_timer_ / 0.35, 0.0, 1.0)) * 0.55f;
    out.rect(0, 0, w, h, rgba(255, 255, 255, uint8_t(alpha * 255.0f)));
  }

  // Bottom controls help bar.
  const float hy0 = h - my - 36 * s, hy1 = h - my - 6 * s;
  out.rect(mx + 6 * s, hy0, w - mx - 6 * s, hy1, kGlass, 4 * s);
  out.text(mx + 18 * s, hy0 + 9 * s,
           "ENTER SNAP PHOTO   TAB EFFECTS   ARROWS ADJUST   WASD/QE MOVE   F AUTOFOCUS   R RESET   H HIDE UI   ESC EXIT",
           TextStyle{.size = 10.5f * s, .weight = 0.11f, .tracking = 0.06f, .color = kWhite, .font = Font::kB});
}

}  // namespace carview
