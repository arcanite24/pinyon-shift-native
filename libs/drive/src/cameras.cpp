#include "drive/cameras.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180;
constexpr double kMph = 2.2369362920544;  // m/s to mph
// A frame this long settles every spring at once (headless frames).
constexpr double kSettle = 0.5;

using V3 = std::array<double, 3>;

V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
V3 normalise(V3 a) {
  const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  return l > 1e-12 ? mul(a, 1 / l) : V3{0, 0, 1};
}
V3 point(const double m[16], V3 p) {
  return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
          m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
V3 vector(const double m[16], V3 p) {
  return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2], m[1] * p[0] + m[5] * p[1] + m[9] * p[2],
          m[2] * p[0] + m[6] * p[1] + m[10] * p[2]};
}

// A direction from yaw (positive right, about +y) and pitch (positive
// down), in a frame whose forward is +z and right +x.
V3 direction(double yaw, double pitch) {
  return {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

// A layer's spring, one implicit step a frame (SPEC-CAMERAS, Spring):
// x' from (x' + dt K (f - x)) / (1 + dt C + dt^2 K), then x += dt x'.
void spring_implicit(double& x, double& v, double target, double k, double c, double dt) {
  if (dt >= kSettle || k <= 0) {
    x = target;
    v = 0;
    return;
  }
  v = (v + dt * k * (target - x)) / (1 + dt * c + dt * dt * k);
  x += dt * v;
}

// --- Parsing ---------------------------------------------------------------

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// The attributes of one element's opening tag (`<Tag a="1" b="2" ...`).
std::map<std::string, std::string> attributes(std::string_view tag) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < tag.size()) {
    const size_t eq = tag.find("=\"", i);
    if (eq == std::string_view::npos) break;
    size_t start = eq;
    while (start > i && tag[start - 1] != ' ' && tag[start - 1] != '\t' && tag[start - 1] != '\n') --start;
    const size_t end = tag.find('"', eq + 2);
    if (end == std::string_view::npos) break;
    out[std::string(tag.substr(start, eq - start))] = std::string(tag.substr(eq + 2, end - eq - 2));
    i = end + 1;
  }
  return out;
}

double number(const std::map<std::string, std::string>& a, const std::string& key, double fallback = 0) {
  auto it = a.find(key);
  return it == a.end() ? fallback : std::strtod(it->second.c_str(), nullptr);
}

}  // namespace

const char* camera_mode_name(CameraMode m) {
  static const char* const kNames[] = {"FollowCam", "FollowCam2", "DriverCam", "Hood", "BumperHigh"};
  return kNames[int(m)];
}

bool parse_camera_settings(std::string_view ini, CameraTuning& out, std::string* error) {
  std::map<std::string, double> v;
  while (!ini.empty()) {
    const size_t nl = ini.find('\n');
    std::string_view line = trim(ini.substr(0, nl));
    ini.remove_prefix(nl == std::string_view::npos ? ini.size() : nl + 1);
    const size_t sp = line.find_first_of(" \t");
    if (line.empty() || sp == std::string_view::npos) continue;
    v[std::string(line.substr(0, sp))] = std::strtod(std::string(trim(line.substr(sp))).c_str(), nullptr);
  }
  bool ok = true;
  auto get = [&](const std::string& key, double& to) {
    auto it = v.find(key);
    if (it == v.end()) {
      if (error && ok) *error = "CameraSettings.ini has no " + key;
      ok = false;
      return;
    }
    to = it->second;
  };
  const char* blocks[2] = {"FollowLowCam\\", "FollowHighCam\\"};
  for (int i = 0; i < 2; ++i) {
    auto& c = out.chase[size_t(i)];
    const std::string b = blocks[i];
    get(b + "HeightOffsetGlobal", c.height);
    get(b + "RadiusOffsetGlobal", c.radius);
    get(b + "TargetYOffsetGlobal", c.target);
    get(b + "HeightMinAboveGround", c.height_min);
    get(b + "HeightMaxAboveGround", c.height_max);
    get(b + "FOV", c.fov);
    get(b + "CamNearClip", c.near_clip);
    get(b + "Spring\\YRotSpringK", c.yaw_k);
    get(b + "Spring\\YRotSpringD", c.yaw_d);
    get(b + "Spring\\YRotCarDampScale", c.yaw_car_damp);
    get(b + "Spring\\YRotMaxSpeed", c.yaw_max_speed);
    get(b + "Spring\\HeightSpringK", c.height_k);
    get(b + "Spring\\HeightSpringD", c.height_d);
  }
  get("Driver\\FOV", out.driver_fov);
  get("Driver\\CamNearClip", out.driver_near);
  get("Driver\\DefaultOffsetFromSteeringWheelX", out.driver_offset[0]);
  get("Driver\\DefaultOffsetFromSteeringWheelY", out.driver_offset[1]);
  get("Driver\\DefaultOffsetFromSteeringWheelZ", out.driver_offset[2]);
  get("HoodCam\\FOV", out.hood_fov);
  get("HoodCam\\CamNearClip", out.hood_near);
  get("HoodCam\\CamYOffset", out.hood_default[0]);
  get("HoodCam\\CamZOffset", out.hood_default[1]);
  get("HoodCam\\CamPitchOffset", out.hood_default[2]);
  get("BumperHighCam\\FOV", out.bumper_fov);
  get("BumperHighCam\\CamNearClip", out.bumper_near);
  get("BumperHighCam\\CamZOffset", out.bumper_z);
  get("BumperHighCam\\RestHeightAboveGround", out.bumper_rest_height);
  return ok;
}

bool parse_camera_physics(std::string_view xml, CameraTuning& out, std::string* error) {
  for (int m = 0; m < kCameraModes; ++m) {
    const std::string name = camera_mode_name(CameraMode(m));
    out.layers[size_t(m)].clear();
    out.layers_left_out[size_t(m)] = 0;
    const size_t open = xml.find("<" + name + " ");
    const size_t close = xml.find("</" + name + ">");
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
      if (error) *error = "CameraPhysics.xml has no " + name;
      return false;
    }
    std::string_view body = xml.substr(open, close - open);
    for (size_t at = body.find("<Layer "); at != std::string_view::npos; at = body.find("<Layer ", at + 1)) {
      const size_t end = body.find('>', at);
      const auto a = attributes(body.substr(at + 7, end - at - 7));
      if (number(a, "Enabled") != 1) continue;
      auto text = [&](const char* k) {
        auto it = a.find(k);
        return it == a.end() ? std::string() : it->second;
      };
      CameraLayer l;
      const std::string input = text("InputType"), output = text("OutputType"), noise = text("NoiseType");
      bool kept = noise == "None" || noise.empty();
      if (input == "SpeedMPH") l.input = CameraLayer::Input::kSpeedMph;
      else if (input == "ConstantOne") l.input = CameraLayer::Input::kConstantOne;
      else if (input == "PitchAngleDegrees") l.input = CameraLayer::Input::kPitchDegrees;
      else if (input == "Gs_Longitudinal") l.input = CameraLayer::Input::kGsLongitudinal;
      else if (input == "Gs_Lateral") l.input = CameraLayer::Input::kGsLateral;
      else kept = false;
      if (output == "CarSpaceXYZOffset") l.output = CameraLayer::Output::kCarXYZ;
      else if (output == "CarSpaceYPROffset") l.output = CameraLayer::Output::kCarYPR;
      else if (output == "CameraSpaceXYZOffset") l.output = CameraLayer::Output::kCameraXYZ;
      else if (output == "CameraSpaceYPROffset") l.output = CameraLayer::Output::kCameraYPR;
      else if (output == "FOV") l.output = CameraLayer::Output::kFov;
      else kept = false;
      if (number(a, "InputTakeDerivative") != 0 || number(a, "InputMagAttackDecay") != 0) kept = false;
      if (!kept) {
        ++out.layers_left_out[size_t(m)];
        continue;
      }
      const std::string mapping = text("InputToParamMapping");
      if (mapping == "ConstantOne") l.mapping = CameraLayer::Mapping::kConstantOne;
      else if (mapping == "Linear") l.mapping = CameraLayer::Mapping::kLinear;
      else l.mapping = CameraLayer::Mapping::kPoints;
      if (a.count("InputClampMin")) l.clamp_min = number(a, "InputClampMin");
      if (a.count("InputClampMax")) l.clamp_max = number(a, "InputClampMax");
      for (int i = 0; i < 5; ++i) {
        const std::string k = "InputToParamCurveInput" + std::to_string(i);
        if (!a.count(k)) break;
        l.points.push_back({number(a, k), number(a, "InputToParamCurveOutput" + std::to_string(i))});
      }
      l.mirrored = number(a, "InputToParamCurveMirrored") != 0;
      l.param = {number(a, "Param.x"), number(a, "Param.y"), number(a, "Param.z"), number(a, "Param.w")};
      l.sprung = number(a, "Sprung") != 0;
      l.spring_k = number(a, "SpringK");
      l.spring_d_percent = number(a, "SpringDPercent");
      out.layers[size_t(m)].push_back(l);
    }
  }
  return true;
}

double CameraCar::override(const std::string& column) const {
  auto it = overrides.find(column);
  return it == overrides.end() ? 0 : it->second;
}

void DriveCamera::set_car(const CameraCar& c) {
  car_ = c;
  started_ = false;
}

void DriveCamera::set_mode(CameraMode m) {
  mode_ = m;
  started_ = false;
}

void DriveCamera::cycle() { set_mode(CameraMode((int(mode_) + 1) % kCameraModes)); }

DriveCamera::LayerOut DriveCamera::step_layers(double dt, const CameraInput& in) {
  const auto& layers = tuning_.layers[size_t(mode_)];
  if (layer_state_.size() != layers.size()) layer_state_.assign(layers.size(), {});
  LayerOut out;
  for (size_t i = 0; i < layers.size(); ++i) {
    const CameraLayer& l = layers[i];
    double x = l.input == CameraLayer::Input::kSpeedMph       ? std::fabs(in.speed) * kMph
               : l.input == CameraLayer::Input::kPitchDegrees ? in.pitch_degrees
               : l.input == CameraLayer::Input::kGsLongitudinal ? in.g_longitudinal
               : l.input == CameraLayer::Input::kGsLateral    ? in.g_lateral
                                                              : 1.0;
    x = std::clamp(x, l.clamp_min, l.clamp_max);
    double s = 1;
    if (l.mapping == CameraLayer::Mapping::kLinear) {
      s = x;
    } else if (l.mapping == CameraLayer::Mapping::kPoints && !l.points.empty()) {
      const double sign = l.mirrored && x < 0 ? -1 : 1;
      const double u = l.mirrored ? std::fabs(x) : x;
      const auto& p = l.points;
      if (u <= p.front().first) s = p.front().second;
      else if (u >= p.back().first) s = p.back().second;
      else
        for (size_t k = 1; k < p.size(); ++k)
          if (u <= p[k].first) {
            const double span = p[k].first - p[k - 1].first;
            const double f = span > 0 ? (u - p[k - 1].first) / span : 1;
            s = p[k - 1].second + f * (p[k].second - p[k - 1].second);
            break;
          }
      s *= sign;
    }
    LayerState& st = layer_state_[i];
    if (l.sprung && st.started) {
      // Damping SpringDPercent of critical (2 sqrt K): the spec's reading.
      spring_implicit(st.x, st.v, s, l.spring_k, l.spring_d_percent * 2 * std::sqrt(l.spring_k), dt);
    } else {
      st.x = s;
      st.v = 0;
    }
    st.started = true;
    const double v = st.x;
    std::array<double, 3>* xyz = nullptr;
    switch (l.output) {
      case CameraLayer::Output::kCarXYZ: xyz = &out.car_xyz; break;
      case CameraLayer::Output::kCarYPR: xyz = &out.car_ypr; break;
      case CameraLayer::Output::kCameraXYZ: xyz = &out.camera_xyz; break;
      case CameraLayer::Output::kCameraYPR: xyz = &out.camera_ypr; break;
      case CameraLayer::Output::kFov: out.fov += l.param[0] * v; break;
    }
    if (xyz)
      for (int k = 0; k < 3; ++k) (*xyz)[size_t(k)] += l.param[size_t(k)] * v;
  }
  return out;
}

void DriveCamera::start(const CameraInput& in) {
  started_ = true;
  layer_state_.clear();
  look_ = look_target_;
  if (mode_ == CameraMode::kFollowLow || mode_ == CameraMode::kFollowHigh) {
    const int i = mode_ == CameraMode::kFollowLow ? 0 : 1;
    const auto& t = tuning_.chase[size_t(i)];
    const std::string p = i == 0 ? "CamFollowLow" : "CamFollowHigh";
    const double h_over = car_.override(p + "HeightOffset");
    const double height = h_over != 0 ? h_over : t.height;
    // The start correction: the eye into the band above the ground under
    // the physics wheel 2 (LR here), only for a car with no override for
    // this camera (any non-zero height, radius or target skips it).
    const bool any_override = h_over != 0 || car_.override(p + "RadiusOffset") != 0 ||
                              car_.override(p + "TargetYOffset") != 0;
    const double start = car_.box_max[1] - in.wheel_ground_y[2] + kEyeLift[i] + height;
    c_ = any_override ? 0
         : start < t.height_min ? t.height_min - start
         : start > t.height_max ? t.height_max - start
                                : 0;
    theta_ = in.heading;
    omega_ = 0;
    h_ = car_.box_max[1] - car_.com[1] + kEyeLift[i];
    h_rate_ = 0;
    slope_hold_ = 0;  // three or more wheels down
  }
  if (mode_ == CameraMode::kBumper) bumper_ground_ = in.wheel_ground_y[0];
}

CameraView DriveCamera::update(double dt, const CameraInput& in) {
  dt = std::fabs(dt);
  if (!started_) {
    start(in);
    dt = kSettle;  // a new camera starts settled
  }
  if (dt >= kSettle) look_ = look_target_;
  else look_ += (look_target_ - look_) * (1 - std::exp(-dt / kLookTime));
  const LayerOut l = step_layers(dt, in);
  CameraView v = mode_ == CameraMode::kFollowLow || mode_ == CameraMode::kFollowHigh ? chase(dt, in, l) : on_car(in, l);
  v.mode = mode_;
  return v;
}

CameraView DriveCamera::chase(double dt, const CameraInput& in, const LayerOut& l) {
  const int i = mode_ == CameraMode::kFollowLow ? 0 : 1;
  const auto& t = tuning_.chase[size_t(i)];
  const std::string p = i == 0 ? "CamFollowLow" : "CamFollowHigh";
  auto pick = [&](const char* column, double global) {
    const double o = car_.override(p + column);
    return o != 0 ? o : global;
  };
  const double height = pick("HeightOffset", t.height), radius = pick("RadiusOffset", t.radius),
               target = pick("TargetYOffset", t.target);
  const V3 com = point(in.car_to_world, car_.com);

  // Heading spring (SPEC-CAMERAS, Q51): the damping acts on the camera's
  // rate relative to s times the car's yaw rate (omega - s psi'); ground
  // contact scales it; YRotMaxSpeed limits it. One step a frame, as the
  // title's update (the replay's fit).
  const double old_yaw = theta_ + look_old_;
  if (dt >= kSettle) {
    theta_ = in.heading;
    omega_ = 0;
  } else {
    const double e = std::remainder(in.heading - theta_, 2 * kPi);
    omega_ += dt * in.contact * (t.yaw_k * e - t.yaw_d * (omega_ - t.yaw_car_damp * in.yaw_rate));
    omega_ = std::clamp(omega_, -t.yaw_max_speed, t.yaw_max_speed);
    theta_ = std::remainder(theta_ + dt * omega_, 2 * kPi);
  }
  look_old_ = look_;

  // Distance: the box's rear behind the CoM, the radius offset and d_base,
  // times sin(F) / sin(F + dF), F the full vertical tuning FOV (Q51).
  const double fov = t.fov + l.fov;
  const double rear = car_.com[2] - car_.box_min[2];
  double r = rear + radius + kBaseDistance[i];
  r *= std::sin(t.fov * kDeg) / std::sin(fov * kDeg);
  const double yaw = theta_ + look_;
  const V3 ahead{std::sin(yaw), 0, std::cos(yaw)};
  V3 eye = add(com, mul(ahead, -r));

  // Height: the box top above the CoM plus the slope term, sprung
  // semi-implicitly. The slope term applies while the car's up axis has
  // n_y > 0.7 and fewer than three wheels have touched for under 0.2 s.
  const V3 up_car = normalise(vector(in.car_to_world, {0, 1, 0}));
  slope_hold_ = in.contact >= 0.75 ? 0.0 : slope_hold_ + dt;
  const bool sloped = up_car[1] > 0.7 && slope_hold_ < kSlopeHold;
  // The car's own plane at the eye's spot (at distance r along `y`),
  // relative to the CoM, limited to +-2 r.
  auto slope_at = [&](double y) {
    if (!sloped) return 0.0;
    const double dx = -r * std::sin(y), dz = -r * std::cos(y);
    return std::clamp(-(up_car[0] * dx + up_car[2] * dz) / up_car[1], -kSlopeLimit * r, kSlopeLimit * r);
  };
  const double slope = slope_at(yaw);
  // Swinging round on a slope moves h at once by the term's change.
  if (dt < kSettle && sloped) h_ += slope - slope_at(old_yaw);
  const double h_target = car_.box_max[1] - car_.com[1] + kEyeLift[i] + slope;
  if (dt >= kSettle || t.height_k <= 0) {
    h_ = h_target;
    h_rate_ = 0;
  } else {
    h_rate_ += dt * (t.height_k * (h_target - h_) - t.height_d * h_rate_);
    h_ += dt * h_rate_;
  }
  eye[1] = com[1] + height + c_ + h_;
  // The view's direction is the chase eye's to the look-at, not re-aimed
  // from the eye the layers move (SPEC-CAMERAS, How the layers compose).
  const V3 look_at = add(com, {0, car_.box_max[1] - car_.com[1] + target + c_, 0});
  V3 f = normalise(add(look_at, mul(eye, -1)));
  // The layers' car-space offsets (the speed pull-back, the pitch lift).
  eye = add(eye, vector(in.car_to_world, l.car_xyz));
  // Clearance (probable): 0.3 m above the world under the eye.
  if (in.ground_below) {
    double gy = 0;
    if (in.ground_below(eye[0], eye[1] + 2.0, eye[2], gy) && eye[1] < gy + kClearance) eye[1] = gy + kClearance;
  }
  // The layers' pitch (positive down), about the view's right axis.
  const double pitch = (l.car_ypr[1] + l.camera_ypr[1]) * kDeg;
  if (pitch != 0) {
    const double horizontal = std::hypot(f[0], f[2]);
    const double a = std::atan2(-f[1], horizontal) + pitch;
    f = {f[0] / std::max(horizontal, 1e-9) * std::cos(a), -std::sin(a), f[2] / std::max(horizontal, 1e-9) * std::cos(a)};
  }
  CameraView v;
  v.eye = eye;
  v.forward = f;
  // Roll: the layers' sum alone (SPEC-CAMERAS, How the layers compose),
  // about the view direction; the car's own roll never reaches the view.
  const double roll = (l.car_ypr[2] + l.camera_ypr[2]) * kDeg;
  V3 up{0, 1, 0};
  if (roll != 0) {
    // The world up made perpendicular to f, turned by roll about f.
    const double d = f[1];
    const V3 u = normalise({-d * f[0], 1 - d * f[1], -d * f[2]});
    const V3 right{f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    up = add(mul(u, std::cos(roll)), mul(right, std::sin(roll)));
  }
  v.up = up;
  v.fov_degrees = fov;
  v.near_clip = t.near_clip;
  return v;
}

CameraView DriveCamera::on_car(const CameraInput& in, const LayerOut& l) {
  V3 eye{};
  double fov = 50, near_clip = 0.1, pitch = 0;
  switch (mode_) {
    case CameraMode::kCockpit: {
      bool any = false;
      for (const char* c : {"CamCockpitOffsetX", "CamCockpitOffsetY", "CamCockpitOffsetZ",
                            "SplitscreenCamCockpitOffsetX", "SplitscreenCamCockpitOffsetY",
                            "SplitscreenCamCockpitOffsetZ", "CockpitWheelPositionOffsetX",
                            "CockpitWheelPositionOffsetY", "CockpitWheelPositionOffsetZ", "CockpitWheelRotationOffset"})
        any = any || car_.override(c) != 0;
      const V3 offset = any ? V3{car_.override("CamCockpitOffsetX"), car_.override("CamCockpitOffsetY"),
                                 car_.override("CamCockpitOffsetZ")}
                            : tuning_.driver_offset;
      eye = add(car_.steering_wheel, offset);
      fov = tuning_.driver_fov;
      near_clip = tuning_.driver_near;
      break;
    }
    case CameraMode::kHood: {
      double y = car_.override("CamHoodHeightOffset"), z = car_.override("CamHoodZOffset");
      double p = car_.override("CamHoodPitchOffset");
      if (y == 0 && z == 0 && p == 0) y = tuning_.hood_default[0], z = tuning_.hood_default[1], p = tuning_.hood_default[2];
      eye = {kHoodAnchor[0], kHoodAnchor[1] + y, kHoodAnchor[2] + z};
      pitch = p;
      fov = tuning_.hood_fov;
      near_clip = tuning_.hood_near;
      break;
    }
    default: {  // the bumper
      const double z_over = car_.override("CamBumperHighZOffset");
      eye = {0, bumper_ground_ + tuning_.bumper_rest_height,
             car_.box_max[2] - kBoxInset + (z_over != 0 ? z_over : tuning_.bumper_z)};
      fov = tuning_.bumper_fov;
      near_clip = tuning_.bumper_near;
      break;
    }
  }
  eye = add(eye, l.car_xyz);
  // Looking back or to a side moves the eye to the box's rear or side.
  const double look = std::remainder(look_, 2 * kPi);
  if (std::fabs(look) > 0.75 * kPi) eye[2] = car_.box_min[2] + kBoxInset;
  else if (std::fabs(look) > 0.25 * kPi) eye[0] = (look > 0 ? 1 : -1) * (car_.box_max[0] - kBoxInset);
  pitch += l.car_ypr[1] + l.camera_ypr[1];
  const double yaw = look + (l.car_ypr[0] + l.camera_ypr[0]) * kDeg;
  CameraView v;
  v.eye = point(in.car_to_world, eye);
  v.forward = normalise(vector(in.car_to_world, direction(yaw, pitch * kDeg)));
  v.up = normalise(vector(in.car_to_world, {0, 1, 0}));
  v.fov_degrees = fov + l.fov;
  v.near_clip = near_clip;
  v.interior = mode_ == CameraMode::kCockpit;
  return v;
}

}  // namespace drive
