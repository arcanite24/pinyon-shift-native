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

double eval_curve(CameraLayer::Mapping mapping, const std::vector<std::pair<double, double>>& points, bool mirrored,
                  double x) {
  if (mapping == CameraLayer::Mapping::kConstantOne) return 1.0;
  if (mapping == CameraLayer::Mapping::kLinear) return x;
  if (points.empty()) return 1.0;
  const double sign = mirrored && x < 0 ? -1.0 : 1.0;
  const double u = mirrored ? std::fabs(x) : x;
  double s = 1.0;
  if (u <= points.front().first) s = points.front().second;
  else if (u >= points.back().first) s = points.back().second;
  else {
    for (size_t k = 1; k < points.size(); ++k) {
      if (u <= points[k].first) {
        const double span = points[k].first - points[k - 1].first;
        const double f = span > 0 ? (u - points[k - 1].first) / span : 1.0;
        s = points[k - 1].second + f * (points[k].second - points[k - 1].second);
        break;
      }
    }
  }
  return s * sign;
}

// Deterministic 1D hash in [-1, 1] for camera shake noise.
double hash1(int64_t n, uint64_t salt) {
  uint64_t x = static_cast<uint64_t>(n) ^ salt;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return static_cast<double>(static_cast<int64_t>(x >> 11)) / static_cast<double>(1LL << 52) - 1.0;
}

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

double eval_noise(CameraLayer::Noise type, double phase) {
  switch (type) {
    case CameraLayer::Noise::kNone: return 1.0;
    case CameraLayer::Noise::kSin: return std::sin(2.0 * kPi * phase);
    case CameraLayer::Noise::kPerlin:
    case CameraLayer::Noise::kSimplex: {
      const uint64_t salt = type == CameraLayer::Noise::kPerlin ? 0x9e3779b97f4a7c15ULL : 0xbf58476d1ce4e5b9ULL;
      const double fl = std::floor(phase);
      const int64_t i0 = static_cast<int64_t>(fl);
      const double t = phase - fl;
      const double g0 = hash1(i0, salt);
      const double g1 = hash1(i0 + 1, salt);
      const double n0 = g0 * t;
      const double n1 = g1 * (t - 1.0);
      const double u = fade(t);
      return std::clamp(2.0 * (n0 + u * (n1 - n0)), -1.0, 1.0);
    }
    case CameraLayer::Noise::kHeightGrid: {
      const double fl = std::floor(phase);
      const int64_t i0 = static_cast<int64_t>(fl);
      const double t = phase - fl;
      const double v0 = hash1(i0, 0x94d049bb133111ebULL);
      const double v1 = hash1(i0 + 1, 0x94d049bb133111ebULL);
      const double u = t * t * (3.0 - 2.0 * t);
      return std::clamp(v0 + u * (v1 - v0), -1.0, 1.0);
    }
  }
  return 0.0;
}

V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

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

const char* driving_view_name(DrivingView v) {
  switch (v) {
    case DrivingView::kNormal: return "normal";
    case DrivingView::kWheel: return "wheel";
    case DrivingView::kReplay: return "replay";
  }
  return "normal";
}

const char* immersive_preset_name(ImmersivePreset p) {
  switch (p) {
    case ImmersivePreset::kOff: return "off";
    case ImmersivePreset::kSubtle: return "subtle";
    case ImmersivePreset::kDefault: return "default";
    case ImmersivePreset::kStrong: return "strong";
    case ImmersivePreset::kCustom: return "custom";
  }
  return "off";
}

ImmersivePreset parse_immersive_preset(std::string_view text) {
  std::string l(text);
  for (char& c : l) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  if (l == "subtle") return ImmersivePreset::kSubtle;
  if (l == "default") return ImmersivePreset::kDefault;
  if (l == "strong") return ImmersivePreset::kStrong;
  if (l == "custom") return ImmersivePreset::kCustom;
  return ImmersivePreset::kOff;
}

void ImmersiveCameraConfig::apply_preset(ImmersivePreset p) {
  double v = 0.0;
  switch (p) {
    case ImmersivePreset::kOff: v = 0.0; break;
    case ImmersivePreset::kSubtle: v = 0.5; break;
    case ImmersivePreset::kDefault: v = 1.0; break;
    case ImmersivePreset::kStrong: v = 1.5; break;
    case ImmersivePreset::kCustom: return;
  }
  gforce = lean = shake = head = v;
}

ImmersivePreset ImmersiveCameraConfig::detect_preset() const {
  auto eq = [](double a, double b) { return std::fabs(a - b) < 1e-3; };
  if (eq(gforce, 0.0) && eq(lean, 0.0) && eq(shake, 0.0) && eq(head, 0.0)) return ImmersivePreset::kOff;
  if (eq(gforce, 0.5) && eq(lean, 0.5) && eq(shake, 0.5) && eq(head, 0.5)) return ImmersivePreset::kSubtle;
  if (eq(gforce, 1.0) && eq(lean, 1.0) && eq(shake, 1.0) && eq(head, 1.0)) return ImmersivePreset::kDefault;
  if (eq(gforce, 1.5) && eq(lean, 1.5) && eq(shake, 1.5) && eq(head, 1.5)) return ImmersivePreset::kStrong;
  return ImmersivePreset::kCustom;
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
      bool kept = true;
      if (noise == "None" || noise.empty()) l.noise = CameraLayer::Noise::kNone;
      else if (noise == "Perlin") l.noise = CameraLayer::Noise::kPerlin;
      else if (noise == "Simplex") l.noise = CameraLayer::Noise::kSimplex;
      else if (noise == "HeightGrid") l.noise = CameraLayer::Noise::kHeightGrid;
      else if (noise == "Sin" || noise == "Sine") l.noise = CameraLayer::Noise::kSin;
      else kept = false;

      if (input == "SpeedMPH") l.input = CameraLayer::Input::kSpeedMph;
      else if (input == "Speed") l.input = CameraLayer::Input::kSpeed;
      else if (input == "ConstantOne") l.input = CameraLayer::Input::kConstantOne;
      else if (input == "OneWhenStationary") l.input = CameraLayer::Input::kOneWhenStationary;
      else if (input == "PitchAngleDegrees") l.input = CameraLayer::Input::kPitchDegrees;
      else if (input == "Gs_Longitudinal") l.input = CameraLayer::Input::kGsLongitudinal;
      else if (input == "Gs_Lateral") l.input = CameraLayer::Input::kGsLateral;
      else if (input == "Slip_Longitudinal") l.input = CameraLayer::Input::kSlipLongitudinal;
      else if (input == "Slip_Lateral") l.input = CameraLayer::Input::kSlipLateral;
      else if (input == "Slip") l.input = CameraLayer::Input::kSlip;
      else if (input == "SurfaceBumpOffset_FL") l.input = CameraLayer::Input::kSurfaceBumpFL;
      else if (input == "SurfaceBumpOffset_FR") l.input = CameraLayer::Input::kSurfaceBumpFR;
      else if (input == "SurfaceBumpOffset_RL") l.input = CameraLayer::Input::kSurfaceBumpRL;
      else if (input == "SurfaceBumpOffset_RR") l.input = CameraLayer::Input::kSurfaceBumpRR;
      else if (input == "SuspensionBias_Longitudinal") l.input = CameraLayer::Input::kSuspensionBiasLongitudinal;
      else if (input == "WorldCollision") l.input = CameraLayer::Input::kWorldCollision;
      else if (input == "WorldCollision_Longitudinal") l.input = CameraLayer::Input::kWorldCollisionLongitudinal;
      else if (input == "WorldCollision_Lateral") l.input = CameraLayer::Input::kWorldCollisionLateral;
      else if (input == "CarCollision") l.input = CameraLayer::Input::kCarCollision;
      else if (input == "CarCollision_Longitudinal") l.input = CameraLayer::Input::kCarCollisionLongitudinal;
      else if (input == "CarCollision_Lateral") l.input = CameraLayer::Input::kCarCollisionLateral;
      else kept = false;

      if (output == "CarSpaceXYZOffset") l.output = CameraLayer::Output::kCarXYZ;
      else if (output == "CarSpaceYPROffset") l.output = CameraLayer::Output::kCarYPR;
      else if (output == "CameraSpaceXYZOffset") l.output = CameraLayer::Output::kCameraXYZ;
      else if (output == "CameraSpaceYPROffset") l.output = CameraLayer::Output::kCameraYPR;
      else if (output == "CarSpaceYPRImpulse") l.output = CameraLayer::Output::kCarYPRImpulse;
      else if (output == "CameraSpaceYPRImpulse") l.output = CameraLayer::Output::kCameraYPRImpulse;
      else if (output == "FOV") l.output = CameraLayer::Output::kFov;
      else if (output == "SteeringWheel") l.output = CameraLayer::Output::kSteeringWheel;
      else kept = false;

      if (!kept) {
        ++out.layers_left_out[size_t(m)];
        continue;
      }
      auto parse_mapping = [](const std::string& m_str) {
        if (m_str == "ConstantOne") return CameraLayer::Mapping::kConstantOne;
        if (m_str == "Linear") return CameraLayer::Mapping::kLinear;
        return CameraLayer::Mapping::kPoints;
      };
      l.mapping = parse_mapping(text("InputToParamMapping"));
      if (a.count("InputClampMin")) l.clamp_min = number(a, "InputClampMin");
      if (a.count("InputClampMax")) l.clamp_max = number(a, "InputClampMax");
      l.take_derivative = number(a, "InputTakeDerivative") != 0;
      l.mag_attack_decay = number(a, "InputMagAttackDecay") != 0;
      l.input_attack = number(a, "InputAttack");
      l.input_decay = number(a, "InputDecay");
      for (int i = 0; i < 5; ++i) {
        const std::string k = "InputToParamCurveInput" + std::to_string(i);
        if (!a.count(k)) break;
        l.points.push_back({number(a, k), number(a, "InputToParamCurveOutput" + std::to_string(i))});
      }
      l.mirrored = number(a, "InputToParamCurveMirrored") != 0;
      if (l.noise != CameraLayer::Noise::kNone) {
        l.noise_freq_mapping = parse_mapping(text("InputToNoiseFreqMapping"));
        for (int i = 0; i < 5; ++i) {
          const std::string k = "InputToNoiseFreqCurveInput" + std::to_string(i);
          if (!a.count(k)) break;
          l.noise_freq_points.push_back({number(a, k), number(a, "InputToNoiseFreqCurveOutput" + std::to_string(i))});
        }
        l.noise_freq_mirrored = number(a, "InputToNoiseFreqCurveMirrored") != 0;
        l.noise_freq = number(a, "NoiseFreq", 1.0);
        l.noise_seed = number(a, "NoiseSeed", 0.0);
        l.noise_range_min = number(a, "NoiseOutputRangeMin", -1.0);
        l.noise_range_max = number(a, "NoiseOutputRangeMax", 1.0);
      }
      l.param = {number(a, "Param.x"), number(a, "Param.y"), number(a, "Param.z"), number(a, "Param.w")};
      l.sprung = number(a, "Sprung") != 0;
      l.spring_k = number(a, "SpringK");
      l.spring_d_percent = number(a, "SpringDPercent");
      l.impulse_cutoff = number(a, "ImpulseInputSizeCutoff");
      l.impulse_delay = number(a, "ImpulseDelay");
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
  if (!car_.has_cockpit_mesh && mode_ == CameraMode::kCockpit) mode_ = CameraMode::kHood;
  started_ = false;
}

void DriveCamera::set_mode(CameraMode m) {
  if (!car_.has_cockpit_mesh && m == CameraMode::kCockpit) m = CameraMode::kHood;
  mode_ = m;
  started_ = false;
}

void DriveCamera::cycle() {
  CameraMode next = CameraMode((int(mode_) + 1) % kCameraModes);
  if (!car_.has_cockpit_mesh && next == CameraMode::kCockpit)
    next = CameraMode((int(next) + 1) % kCameraModes);
  set_mode(next);
}

DriveCamera::LayerOut DriveCamera::step_layers(double dt, const CameraInput& in) {
  const auto& layers = tuning_.layers[size_t(mode_)];
  if (layer_state_.size() != layers.size()) layer_state_.assign(layers.size(), {});
  LayerOut out;
  for (size_t i = 0; i < layers.size(); ++i) {
    const CameraLayer& l = layers[i];
    double x = 1.0;
    switch (l.input) {
      case CameraLayer::Input::kSpeedMph: x = std::fabs(in.speed) * kMph; break;
      case CameraLayer::Input::kSpeed: x = std::clamp(std::fabs(in.speed) * kMph / 200.0, 0.0, 1.0); break;
      case CameraLayer::Input::kConstantOne: x = 1.0; break;
      case CameraLayer::Input::kOneWhenStationary: x = std::fabs(in.speed) < 0.5 ? 1.0 : 0.0; break;
      case CameraLayer::Input::kPitchDegrees: x = in.pitch_degrees; break;
      case CameraLayer::Input::kGsLongitudinal: x = in.g_longitudinal; break;
      case CameraLayer::Input::kGsLateral: x = in.g_lateral; break;
      case CameraLayer::Input::kSlipLongitudinal: x = in.slip_longitudinal; break;
      case CameraLayer::Input::kSlipLateral: x = in.slip_lateral; break;
      case CameraLayer::Input::kSlip: x = std::hypot(in.slip_longitudinal, in.slip_lateral); break;
      case CameraLayer::Input::kSurfaceBumpFL: x = in.surface_bump[0]; break;
      case CameraLayer::Input::kSurfaceBumpFR: x = in.surface_bump[1]; break;
      case CameraLayer::Input::kSurfaceBumpRL: x = in.surface_bump[2]; break;
      case CameraLayer::Input::kSurfaceBumpRR: x = in.surface_bump[3]; break;
      case CameraLayer::Input::kSuspensionBiasLongitudinal: x = in.suspension_bias_longitudinal; break;
      case CameraLayer::Input::kWorldCollision:
        x = std::hypot(in.world_collision_longitudinal, in.world_collision_lateral);
        break;
      case CameraLayer::Input::kWorldCollisionLongitudinal: x = in.world_collision_longitudinal; break;
      case CameraLayer::Input::kWorldCollisionLateral: x = in.world_collision_lateral; break;
      case CameraLayer::Input::kCarCollision:
        x = std::hypot(in.car_collision_longitudinal, in.car_collision_lateral);
        break;
      case CameraLayer::Input::kCarCollisionLongitudinal: x = in.car_collision_longitudinal; break;
      case CameraLayer::Input::kCarCollisionLateral: x = in.car_collision_lateral; break;
    }
    x = std::clamp(x, l.clamp_min, l.clamp_max);
    LayerState& st = layer_state_[i];
    if (l.take_derivative) {
      const double raw = x;
      x = (!st.started || dt >= kSettle || dt <= 1e-6) ? 0.0 : (raw - st.prev_in) / dt;
      st.prev_in = raw;
    }
    if (l.mag_attack_decay) {
      const double mag = std::fabs(x);
      if (mag > 1e-9) st.env_sign = x < 0 ? -1.0 : 1.0;
      if (!st.started || dt >= kSettle) {
        st.env = mag;
      } else if (mag > st.env) {
        st.env = std::min(mag, st.env + l.input_attack * dt);
      } else {
        st.env = std::max(mag, st.env - l.input_decay * dt);
      }
      x = st.env * st.env_sign;
    }

    double s = eval_curve(l.mapping, l.points, l.mirrored, x);

    if (l.noise != CameraLayer::Noise::kNone) {
      if (!st.started) st.noise_phase = l.noise_seed;
      const double f_mult = eval_curve(l.noise_freq_mapping, l.noise_freq_points, l.noise_freq_mirrored, x);
      if (dt < kSettle) st.noise_phase += dt * l.noise_freq * f_mult;
      const double n = eval_noise(l.noise, st.noise_phase);
      const double mapped = l.noise_range_min + 0.5 * (n + 1.0) * (l.noise_range_max - l.noise_range_min);
      s *= mapped;
    }

    const bool is_impulse =
        l.output == CameraLayer::Output::kCarYPRImpulse || l.output == CameraLayer::Output::kCameraYPRImpulse;
    if (is_impulse) {
      if (!st.started || dt >= kSettle) {
        st.x = 0;
        st.v = 0;
        st.impulse_timer = -1.0;
        st.pending_impulse = 0.0;
      } else {
        if (std::fabs(x) >= l.impulse_cutoff && std::fabs(s) > 1e-9) {
          if (l.impulse_delay > 0.0) {
            st.pending_impulse = s;
            st.impulse_timer = l.impulse_delay;
          } else {
            st.v += s;
          }
        }
        if (st.impulse_timer >= 0.0) {
          st.impulse_timer -= dt;
          if (st.impulse_timer <= 0.0) {
            st.v += st.pending_impulse;
            st.pending_impulse = 0.0;
            st.impulse_timer = -1.0;
          }
        }
        const double k = l.spring_k > 0 ? l.spring_k : 100.0;
        const double d_pct = l.spring_d_percent > 0 ? l.spring_d_percent : 0.7;
        spring_implicit(st.x, st.v, 0.0, k, d_pct * 2.0 * std::sqrt(k), dt);
      }
    } else if (l.sprung && st.started) {
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
      case CameraLayer::Output::kCarYPR:
      case CameraLayer::Output::kCarYPRImpulse: xyz = &out.car_ypr; break;
      case CameraLayer::Output::kCameraXYZ: xyz = &out.camera_xyz; break;
      case CameraLayer::Output::kCameraYPR:
      case CameraLayer::Output::kCameraYPRImpulse: xyz = &out.camera_ypr; break;
      case CameraLayer::Output::kFov: out.fov += l.param[0] * v; break;
      case CameraLayer::Output::kSteeringWheel: out.steering_wheel += l.param[0] * v * kDeg; break;
    }
    if (xyz)
      for (int k = 0; k < 3; ++k) (*xyz)[size_t(k)] += l.param[size_t(k)] * v;
  }
  return out;
}

void DriveCamera::start(const CameraInput& in) {
  started_ = true;
  layer_state_.clear();
  imm_g_long_ = 0;
  imm_g_lat_ = 0;
  imm_prev_g_long_ = in.g_longitudinal;
  imm_jolt_ = 0;
  imm_jolt_v_ = 0;
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
    wall_pull_ = 1.0;
  }
  if (mode_ == CameraMode::kBumper) bumper_ground_ = in.wheel_ground_y[0];
}

DriveCamera::LayerOut DriveCamera::step_immersive(double dt, const CameraInput& in) {
  LayerOut out;
  const double gf = std::clamp(immersive_.gforce, 0.0, 2.0);
  const double ln = std::clamp(immersive_.lean, 0.0, 2.0);
  const double sh = std::clamp(immersive_.shake, 0.0, 2.0);
  const double hd = std::clamp(immersive_.head, 0.0, 2.0);
  if (gf <= 0.0 && ln <= 0.0 && sh <= 0.0 && hd <= 0.0) return out;

  const bool is_cockpit = mode_ == CameraMode::kCockpit;
  const bool is_hood = mode_ == CameraMode::kHood;
  const bool is_chase_or_bumper = !is_cockpit && !is_hood;
  // Cockpit gets full effect, hood gets 70% of lean/shake/head, chase & bumper get G-forces only.
  const double cabin_scale = is_cockpit ? 1.0 : (is_hood ? 0.70 : 0.0);

  if (dt >= kSettle) {
    imm_g_long_ = std::clamp(in.g_longitudinal, -2.5, 2.5);
    imm_g_lat_ = std::clamp(in.g_lateral, -2.5, 2.5);
    imm_prev_g_long_ = imm_g_long_;
    imm_jolt_ = 0.0;
    imm_jolt_v_ = 0.0;
  } else {
    const double a = 1.0 - std::exp(-dt * 9.0);
    const double gl_raw = std::clamp(in.g_longitudinal, -2.5, 2.5);
    const double gla_raw = std::clamp(in.g_lateral, -2.5, 2.5);
    imm_g_long_ += (gl_raw - imm_g_long_) * a;
    imm_g_lat_ += (gla_raw - imm_g_lat_) * a;
    // Throttle/brake onset jolt from longitudinal G derivative.
    const double dg = dt > 1e-5 ? std::clamp((gl_raw - imm_prev_g_long_) / dt, -25.0, 25.0) : 0.0;
    imm_prev_g_long_ = gl_raw;
    if (std::fabs(dg) > 2.0) imm_jolt_v_ += std::clamp(dg * 0.015, -0.4, 0.4);
    spring_implicit(imm_jolt_, imm_jolt_v_, 0.0, 65.0, 2.0 * 0.65 * std::sqrt(65.0), dt);
    imm_phase_ += dt;
  }

  // 1. G-Force push-back under acceleration, throw-forward under braking, and onset jolt.
  if (gf > 0.0) {
    const double z_gain = is_chase_or_bumper ? 0.14 : 0.045;
    // Positive g_longitudinal (accelerating) pushes camera back (-Z); braking throws forward (+Z).
    out.camera_xyz[2] += (-imm_g_long_ * z_gain - imm_jolt_ * (is_chase_or_bumper ? 0.10 : 0.03)) * gf;
    // Pitch dive under braking, slight squat under acceleration.
    out.camera_ypr[1] += (-imm_g_long_ * (is_chase_or_bumper ? 0.65 : 1.15) - imm_jolt_ * 1.6) * gf;
    // Slight lateral shift in chase/cockpit under lateral G.
    out.camera_xyz[0] += (-imm_g_lat_ * (is_chase_or_bumper ? 0.10 : 0.028)) * gf;
  }

  // 2. Corner lean (roll & subtle look-into-apex yaw in cockpit & hood).
  if (ln > 0.0 && cabin_scale > 0.0) {
    out.camera_ypr[2] += (imm_g_lat_ * 2.4) * ln * cabin_scale;
    out.camera_ypr[0] += (imm_g_lat_ * 1.6) * ln * cabin_scale;
  }

  // 3. Speed & hard-braking micro-shake (cockpit & hood).
  if (sh > 0.0 && cabin_scale > 0.0) {
    const double speed_n = std::clamp((std::fabs(in.speed) * kMph - 25.0) / 150.0, 0.0, 1.0);
    const double brake_hard = std::clamp((-imm_g_long_ - 0.55) / 1.0, 0.0, 1.0);
    const double amp = (0.0045 * speed_n * speed_n + 0.0055 * brake_hard) * sh * cabin_scale;
    const double n1 = eval_noise(CameraLayer::Noise::kPerlin, imm_phase_ * (14.0 + 18.0 * speed_n));
    const double n2 = eval_noise(CameraLayer::Noise::kSimplex, imm_phase_ * (17.0 + 20.0 * speed_n) + 13.7);
    out.camera_xyz[0] += n1 * amp;
    out.camera_xyz[1] += n2 * amp;
    out.camera_ypr[1] += n2 * (0.28 * speed_n + 0.45 * brake_hard) * sh * cabin_scale;
  }

  // 4. Slow cockpit breathing & subtle head sway.
  if (hd > 0.0 && cabin_scale > 0.0) {
    const double breath = std::sin(2.0 * kPi * 0.24 * imm_phase_);
    const double sway = std::sin(2.0 * kPi * 0.17 * imm_phase_ + 0.8);
    out.camera_xyz[1] += 0.0045 * breath * hd * cabin_scale;
    out.camera_xyz[0] += 0.0040 * sway * hd * cabin_scale;
    out.camera_ypr[2] += 0.35 * sway * hd * cabin_scale;
  }

  return out;
}

CameraView DriveCamera::special_view(const CameraInput& in, const LayerOut& l) {
  CameraView v;
  V3 eye_local{};
  double yaw = 0.0, pitch = 0.0, fov = 56.0;
  if (editor_.driving_view == DrivingView::kWheel) {
    // Front-left wheel-arch driving camera looking forward past the front tyre.
    eye_local = {car_.box_min[0] - 0.28, car_.box_min[1] + 0.48, car_.com[2] - 0.15};
    yaw = 0.06;
    pitch = 1.5;
    fov = 60.0;
  } else {
    // Car-relative dynamic low 3/4 replay driving view.
    eye_local = {car_.box_max[0] + 1.35, car_.box_max[1] + 0.35, car_.box_min[2] - 2.35};
    yaw = -0.32;
    pitch = 4.0;
    fov = 48.0;
  }
  eye_local = add(eye_local, l.car_xyz);
  v.eye = point(in.car_to_world, eye_local);
  v.forward = normalise(vector(in.car_to_world, direction(yaw + (l.car_ypr[0] + l.camera_ypr[0]) * kDeg,
                                                          (pitch + l.car_ypr[1] + l.camera_ypr[1]) * kDeg)));
  v.up = normalise(vector(in.car_to_world, {0, 1, 0}));
  v.fov_degrees = fov + l.fov;
  v.near_clip = 0.2;
  v.mode = mode_;
  v.interior = false;
  return v;
}

CameraView DriveCamera::update(double dt, const CameraInput& in) {
  dt = std::fabs(dt);
  if (!started_) {
    start(in);
    dt = kSettle;  // a new camera starts settled
  }
  if (dt >= kSettle) look_ = look_target_;
  else look_ += (look_target_ - look_) * (1 - std::exp(-dt / kLookTime));
  LayerOut l = step_layers(dt, in);
  const LayerOut imm = step_immersive(dt, in);
  for (size_t k = 0; k < 3; ++k) {
    l.car_xyz[k] += imm.car_xyz[k];
    l.car_ypr[k] += imm.car_ypr[k];
    l.camera_xyz[k] += imm.camera_xyz[k];
    l.camera_ypr[k] += imm.camera_ypr[k];
  }
  l.fov += imm.fov;

  CameraView v;
  if (editor_.driving_view != DrivingView::kNormal) {
    v = special_view(in, l);
  } else {
    v = mode_ == CameraMode::kFollowLow || mode_ == CameraMode::kFollowHigh ? chase(dt, in, l) : on_car(in, l);
  }
  v.mode = mode_;
  v.steering_wheel_kick = l.steering_wheel;
  if (editor_.driving_view == DrivingView::kNormal && !head_pose_.is_zero()) {
    apply_head_tracking(v, head_pose_, mode_);
  }
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
  const double height = pick("HeightOffset", t.height) + editor_.chase_height_m;
  const double radius = pick("RadiusOffset", t.radius) + editor_.chase_distance_m;
  const double target = pick("TargetYOffset", t.target);
  const double stiff = std::clamp(editor_.chase_stiffness, 0.5, 2.0);
  const double stiff_d = std::sqrt(stiff);  // preserves damping ratio
  const double fov_scale = std::clamp(editor_.chase_fov_scale, 0.8, 1.3);
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
    omega_ += dt * in.contact * (t.yaw_k * stiff * e - t.yaw_d * stiff_d * (omega_ - t.yaw_car_damp * in.yaw_rate));
    omega_ = std::clamp(omega_, -t.yaw_max_speed, t.yaw_max_speed);
    theta_ = std::remainder(theta_ + dt * omega_, 2 * kPi);
  }
  look_old_ = look_;

  // Distance: the box's rear behind the CoM, the radius offset and d_base,
  // times sin(F) / sin(F + dF), F the full vertical tuning FOV (Q51).
  const double base_fov = t.fov * fov_scale;
  const double fov = base_fov + l.fov;
  const double rear = car_.com[2] - car_.box_min[2];
  double r = rear + radius + kBaseDistance[i];
  r *= std::sin(base_fov * kDeg) / std::sin(fov * kDeg);
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
    h_rate_ += dt * (t.height_k * stiff * (h_target - h_) - t.height_d * stiff_d * h_rate_);
    h_ += dt * h_rate_;
  }
  eye[1] = com[1] + height + c_ + h_;
  // The view's direction is the chase eye's to the look-at, not re-aimed
  // from the eye the layers move (SPEC-CAMERAS, How the layers compose).
  const V3 look_at = add(com, {0, car_.box_max[1] - car_.com[1] + target + c_, 0});
  V3 f = normalise(add(look_at, mul(eye, -1)));
  // The layers' car-space offsets (the speed pull-back, the pitch lift),
  // plus camera-space offsets along the view's right/up/forward axes.
  eye = add(eye, vector(in.car_to_world, l.car_xyz));
  if (l.camera_xyz[0] != 0 || l.camera_xyz[1] != 0 || l.camera_xyz[2] != 0) {
    const V3 cam_right = normalise(cross({0, 1, 0}, f));
    const V3 cam_up = normalise(cross(f, cam_right));
    eye = add(eye, add(mul(cam_right, l.camera_xyz[0]),
                       add(mul(cam_up, l.camera_xyz[1]), mul(f, l.camera_xyz[2]))));
  }
  // Horizontal wall / car-OBB obstacle pull-in (CAMERAS.md:266): raycast from
  // look_at toward eye; pull in immediately when blocked, ease back smoothly.
  if (in.obstacle_ray) {
    const V3 to_eye = add(eye, mul(look_at, -1.0));
    const double full_dist = std::sqrt(to_eye[0] * to_eye[0] + to_eye[1] * to_eye[1] + to_eye[2] * to_eye[2]);
    if (full_dist > 0.5) {
      const V3 dir = mul(to_eye, 1.0 / full_dist);
      double hit_d = full_dist;
      double target_pull = 1.0;
      if (in.obstacle_ray(look_at, dir, full_dist, hit_d) && hit_d < full_dist) {
        const double safe_d = std::max(0.6, hit_d - 0.35);
        target_pull = std::clamp(safe_d / full_dist, 0.2, 1.0);
      }
      if (dt >= kSettle || target_pull < wall_pull_) {
        wall_pull_ = target_pull;
      } else {
        wall_pull_ += (target_pull - wall_pull_) * (1.0 - std::exp(-dt * 6.0));
      }
      if (wall_pull_ < 0.999) {
        eye = add(look_at, mul(to_eye, wall_pull_));
      }
    }
  }
  // Clearance (probable): 0.3 m above the world under the eye.
  if (in.ground_below) {
    double gy = 0;
    if (in.ground_below(eye[0], eye[1] + 2.0, eye[2], gy) && eye[1] < gy + kClearance) eye[1] = gy + kClearance;
  }
  // The layers' yaw (positive right) and pitch (positive down).
  const double layer_yaw = (l.car_ypr[0] + l.camera_ypr[0]) * kDeg;
  if (layer_yaw != 0) {
    const double cy = std::cos(layer_yaw), sy = std::sin(layer_yaw);
    f = {f[0] * cy + f[2] * sy, f[1], -f[0] * sy + f[2] * cy};
  }
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
      near_clip = p != 0.0 ? tuning_.hood_near * 0.5 : tuning_.hood_near;
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
  // Looking back or to a side smoothly blends the eye to the box's rear or side
  // (exact match at 0, +-pi/2, and +-pi without a discontinuous jump).
  const double look = std::remainder(look_, 2 * kPi);
  const double alook = std::fabs(look);
  auto smoothstep = [](double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
  };
  const double w_back = smoothstep(0.55 * kPi, 0.85 * kPi, alook);
  const double w_side = smoothstep(0.15 * kPi, 0.45 * kPi, alook) * (1.0 - w_back);
  if (w_back > 0.0) {
    const double back_z = car_.box_min[2] + kBoxInset;
    eye[2] = eye[2] + (back_z - eye[2]) * w_back;
  }
  if (w_side > 0.0) {
    const double side_x = (look > 0 ? 1.0 : -1.0) * (car_.box_max[0] - kBoxInset);
    eye[0] = eye[0] + (side_x - eye[0]) * w_side;
  }
  pitch += l.car_ypr[1] + l.camera_ypr[1];
  const double yaw = look + (l.car_ypr[0] + l.camera_ypr[0]) * kDeg;
  CameraView v;
  v.eye = point(in.car_to_world, eye);
  v.forward = normalise(vector(in.car_to_world, direction(yaw, pitch * kDeg)));
  V3 up = normalise(vector(in.car_to_world, {0, 1, 0}));
  if (l.camera_xyz[0] != 0 || l.camera_xyz[1] != 0 || l.camera_xyz[2] != 0) {
    const V3 cam_right = normalise(cross(up, v.forward));
    const V3 cam_up = normalise(cross(v.forward, cam_right));
    v.eye = add(v.eye, add(mul(cam_right, l.camera_xyz[0]),
                           add(mul(cam_up, l.camera_xyz[1]), mul(v.forward, l.camera_xyz[2]))));
  }
  const double roll = (l.car_ypr[2] + l.camera_ypr[2]) * kDeg;
  if (roll != 0) {
    const double d = v.forward[0] * up[0] + v.forward[1] * up[1] + v.forward[2] * up[2];
    const V3 u = normalise({up[0] - d * v.forward[0], up[1] - d * v.forward[1], up[2] - d * v.forward[2]});
    const V3 right = cross(v.forward, u);
    up = add(mul(u, std::cos(roll)), mul(right, std::sin(roll)));
  }
  v.up = up;
  v.fov_degrees = fov + l.fov;
  v.near_clip = near_clip;
  v.interior = mode_ == CameraMode::kCockpit;
  return v;
}

}  // namespace drive
