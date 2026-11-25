#include "world/tod.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "fh1data/archive.h"
#include "fh1data/bytes.h"
#include "world/world.h"

namespace world {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); ++i) out[i] = char(std::tolower(static_cast<unsigned char>(s[i])));
  return out;
}

bool read_text_file(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

bool read_binary_file(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const auto sz = in.tellg();
  if (sz < 0) return false;
  out.resize(size_t(sz));
  in.seekg(0);
  return bool(in.read(reinterpret_cast<char*>(out.data()), sz));
}

// Lightweight self-contained XML tree parser for TOD / TrackSettings / Zones / Templates.
struct XmlElem {
  std::string tag;
  std::map<std::string, std::string> attrs;  // original case
  std::vector<XmlElem> children;

  const XmlElem* child(std::string_view name) const {
    for (const auto& c : children)
      if (c.tag == name) return &c;
    return nullptr;
  }
  const XmlElem* child_ci(std::string_view name) const {
    const std::string want = lower_ascii(name);
    for (const auto& c : children)
      if (lower_ascii(c.tag) == want) return &c;
    return nullptr;
  }
  std::string attr(std::string_view name, std::string_view fallback = "") const {
    auto it = attrs.find(std::string(name));
    if (it != attrs.end()) return it->second;
    const std::string want = lower_ascii(name);
    for (const auto& [k, v] : attrs)
      if (lower_ascii(k) == want) return v;
    return std::string(fallback);
  }
  float num(std::string_view name, float fallback = 0.0f) const {
    const std::string s = attr(name);
    if (s.empty()) return fallback;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
  }
};

struct XmlScanner {
  std::string_view s;
  size_t pos = 0;

  void skip_ws() {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
  }
  void skip_misc() {
    while (pos < s.size()) {
      skip_ws();
      if (pos + 4 <= s.size() && s.substr(pos, 4) == "<!--") {
        const size_t end = s.find("-->", pos + 4);
        pos = end == std::string_view::npos ? s.size() : end + 3;
      } else if (pos + 2 <= s.size() && s.substr(pos, 2) == "<?") {
        const size_t end = s.find("?>", pos + 2);
        pos = end == std::string_view::npos ? s.size() : end + 2;
      } else {
        break;
      }
    }
  }
  std::string read_name() {
    const size_t start = pos;
    while (pos < s.size()) {
      const unsigned char c = static_cast<unsigned char>(s[pos]);
      if (std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.') ++pos;
      else break;
    }
    return std::string(s.substr(start, pos - start));
  }
  bool parse_elem(XmlElem& out) {
    skip_misc();
    if (pos >= s.size() || s[pos] != '<') return false;
    if (pos + 1 < s.size() && s[pos + 1] == '/') return false;
    ++pos;  // '<'
    out.tag = read_name();
    if (out.tag.empty()) return false;
    while (pos < s.size()) {
      skip_ws();
      if (pos >= s.size()) return false;
      if (s[pos] == '/' && pos + 1 < s.size() && s[pos + 1] == '>') {
        pos += 2;
        return true;
      }
      if (s[pos] == '>') {
        ++pos;
        break;
      }
      std::string k = read_name();
      if (k.empty()) {
        ++pos;
        continue;
      }
      skip_ws();
      std::string v;
      if (pos < s.size() && s[pos] == '=') {
        ++pos;
        skip_ws();
        if (pos < s.size() && (s[pos] == '"' || s[pos] == '\'')) {
          const char q = s[pos++];
          const size_t start = pos;
          while (pos < s.size() && s[pos] != q) ++pos;
          v = std::string(s.substr(start, pos - start));
          if (pos < s.size() && s[pos] == q) ++pos;
        }
      }
      out.attrs[std::move(k)] = std::move(v);
    }
    // Parse children until matching closing tag.
    while (pos < s.size()) {
      skip_misc();
      if (pos >= s.size()) break;
      if (s[pos] != '<') {
        const size_t next = s.find('<', pos);
        pos = next == std::string_view::npos ? s.size() : next;
        continue;
      }
      if (pos + 1 < s.size() && s[pos + 1] == '/') {
        pos += 2;
        const size_t close = s.find('>', pos);
        pos = close == std::string_view::npos ? s.size() : close + 1;
        return true;
      }
      XmlElem child;
      if (!parse_elem(child)) break;
      out.children.push_back(std::move(child));
    }
    return true;
  }
};

bool parse_xml_root(std::string_view text, XmlElem& root) {
  // Strip UTF-8 BOM if present.
  if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF &&
      static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }
  XmlScanner sc{text, 0};
  return sc.parse_elem(root);
}

std::array<float, 3> normalize3(std::array<float, 3> v, std::array<float, 3> fallback = {0.0f, 1.0f, 0.0f}) {
  const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (len2 < 1e-10f) return fallback;
  const float inv = 1.0f / std::sqrt(len2);
  return {v[0] * inv, v[1] * inv, v[2] * inv};
}

std::array<float, 3> sub3(std::array<float, 3> a, std::array<float, 3> b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

uint32_t next_u32(uint32_t& state) {
  uint32_t x = state ? state : 0x51A7E3C1u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

float read_be_f32(const uint8_t* p) {
  const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                        (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

bool decode_sky_xds(fh1::Archive& arch, const char* member, fh1::RgbaImage& out, std::string* error) {
  const auto res = arch.read(member);
  if (!res.ok()) {
    if (error) *error = std::string("realtimesky.zip: ") + member + ": " + res.error;
    return false;
  }
  const fh1::XdsResult decoded = fh1::decode_xds(res.data, false);
  if (decoded.levels.empty()) {
    if (error) *error = std::string("realtimesky.zip: ") + member + ": " + decoded.error;
    return false;
  }
  out = decoded.levels[0];
  return true;
}

}  // namespace

// SPEC-SKY-AND-TOD §2.4: piecewise-linear between the two bracketing keys;
// times outside [first key, last key] take the nearest end key (no wrap
// across midnight).
float TodCurve::eval_scalar(float time_min, float fallback) const {
  if (keys.empty()) return fallback;
  if (keys.size() == 1) return keys.front().v[0];
  if (time_min <= keys.front().time_min) return keys.front().v[0];
  if (time_min >= keys.back().time_min) return keys.back().v[0];
  auto it = std::upper_bound(keys.begin(), keys.end(), time_min,
                             [](float t, const Key& k) { return t < k.time_min; });
  if (it == keys.begin()) return keys.front().v[0];
  const Key& k1 = *it;
  const Key& k0 = *(it - 1);
  const float span = k1.time_min - k0.time_min;
  const float f = span > 1e-6f ? (time_min - k0.time_min) / span : 0.0f;
  return k0.v[0] + f * (k1.v[0] - k0.v[0]);
}

std::array<float, 3> TodCurve::eval_vec3(float time_min, std::array<float, 3> fallback) const {
  if (keys.empty()) return fallback;
  if (keys.size() == 1) return {keys.front().v[0], keys.front().v[1], keys.front().v[2]};
  if (time_min <= keys.front().time_min) return {keys.front().v[0], keys.front().v[1], keys.front().v[2]};
  if (time_min >= keys.back().time_min) return {keys.back().v[0], keys.back().v[1], keys.back().v[2]};
  auto it = std::upper_bound(keys.begin(), keys.end(), time_min,
                             [](float t, const Key& k) { return t < k.time_min; });
  if (it == keys.begin()) return {keys.front().v[0], keys.front().v[1], keys.front().v[2]};
  const Key& k1 = *it;
  const Key& k0 = *(it - 1);
  const float span = k1.time_min - k0.time_min;
  const float f = span > 1e-6f ? (time_min - k0.time_min) / span : 0.0f;
  return {k0.v[0] + f * (k1.v[0] - k0.v[0]),
          k0.v[1] + f * (k1.v[1] - k0.v[1]),
          k0.v[2] + f * (k1.v[2] - k0.v[2])};
}

bool TodPreset::load_xml(const std::string& xml_text, std::string* error) {
  XmlElem root;
  if (!parse_xml_root(xml_text, root) || lower_ascii(root.tag) != "tod") {
    if (error) *error = "not a <TOD> document";
    return false;
  }
  curves.clear();
  for (const auto& ch : root.children) {
    TodCurve curve;
    curve.keys.reserve(ch.children.size());
    for (const auto& k : ch.children) {
      if (lower_ascii(k.tag) != "key") continue;
      TodCurve::Key key;
      key.time_min = k.num("time", 0.0f);
      if (k.attrs.count("r")) {
        key.v[0] = k.num("r", 0.0f);
        key.v[1] = k.num("g", 0.0f);
        key.v[2] = k.num("b", 0.0f);
      } else {
        key.v[0] = k.num("x", 0.0f);
        key.v[1] = k.num("y", 0.0f);
        key.v[2] = k.num("z", 0.0f);
      }
      curve.keys.push_back(key);
    }
    std::stable_sort(curve.keys.begin(), curve.keys.end(),
                     [](const TodCurve::Key& a, const TodCurve::Key& b) { return a.time_min < b.time_min; });
    curves[ch.tag] = std::move(curve);
  }
  return !curves.empty();
}

bool TodPreset::load_file(const std::filesystem::path& path, std::string* error) {
  std::string text;
  if (!read_text_file(path, text)) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  return load_xml(text, error);
}

void TodPreset::merge_fallback(const TodPreset& fallback) {
  for (const auto& [name, curve] : fallback.curves) {
    auto it = curves.find(name);
    if (it == curves.end() || it->second.keys.empty()) curves[name] = curve;
  }
}

float TodPreset::scalar(const std::string& name, float time_min, float fallback) const {
  auto it = curves.find(name);
  return it == curves.end() ? fallback : it->second.eval_scalar(time_min, fallback);
}

std::array<float, 3> TodPreset::vec3(const std::string& name, float time_min,
                                     std::array<float, 3> fallback) const {
  auto it = curves.find(name);
  return it == curves.end() ? fallback : it->second.eval_vec3(time_min, fallback);
}

void TodClock::set_hour(double h) {
  double wrapped = std::fmod(std::fmod(h, 24.0) + 24.0, 24.0);
  tod_sec = wrapped * 3600.0;
}

void TodClock::step_hours(double delta_hours) {
  const double prev = tod_sec;
  const double next = std::fmod(std::fmod(tod_sec + delta_hours * 3600.0, 86400.0) + 86400.0, 86400.0);
  if (delta_hours > 0.0 && prev < 43200.0 && next >= 43200.0) ++day_count;
  if (delta_hours < 0.0 && prev >= 43200.0 && next < 43200.0) --day_count;
  tod_sec = next;
}

void TodClock::request(double target_sec, double rate) {
  request_rate = rate;
  request_floor = tod_sec;
  rate_override = true;
  request_target = target_sec;
  transition = true;
  paused = false;
}

void TodClock::advance(double dt, double curve_time_speed) {
  if (paused || dt == 0.0) return;
  const double speed = rate_override ? request_rate : curve_time_speed * speed_scale;
  if (speed == 0.0) return;
  const double prev = tod_sec;
  double next = prev + speed * dt;
  // 20:00 (72,000 s) crossing rolls the next day's A/B preset (30% B, 70% A).
  if (!force_preset && prev < 72000.0 && next >= 72000.0) {
    const double u = double(next_u32(rng_state)) / 4294967296.0;
    use_preset_b = (u < 0.30);
  }
  bool wrapped_day = false;
  if (next > 86400.0) {
    next -= 86400.0;
    if (transition) request_target -= 86400.0;
    wrapped_day = true;
  } else if (next < 0.0) {
    next += 86400.0;
    if (transition) request_target += 86400.0;
  }
  // Noon crossing advances the lunar phase counter.
  const bool crossed_noon = (!wrapped_day && prev < 43200.0 && next >= 43200.0) ||
                            (wrapped_day && (prev < 43200.0 || next >= 43200.0));
  if (crossed_noon) ++day_count;
  if (transition) {
    if (next >= request_target) {
      transition = false;
      paused = true;
      rate_override = false;
      next = request_target;
    } else if (next < request_floor) {
      next = request_floor;
    }
  }
  tod_sec = next;
}

TodState evaluate_tod(const TodPreset& p, double tod_sec) {
  TodState s;
  double wrapped = std::fmod(std::fmod(tod_sec, 86400.0) + 86400.0, 86400.0);
  s.tod_sec = float(wrapped);
  const float m = float(wrapped / 60.0);
  s.tod_min = m;

  // Directional body and sky disc positions (in game frame).
  const auto sun_pos = p.vec3("SunPos", m, {42.9f, 65.9f, 37.2f});
  const auto sun_target = p.vec3("SunTargetPos", m, {24.633f, 0.0f, 45.088f});
  s.sun_dir = normalize3(sub3(sun_pos, sun_target), {0.3848f, 0.8754f, 0.2926f});

  const auto obj_pos = p.vec3("SunObjectPos", m, sun_pos);
  const auto obj_target = p.vec3("SunObjectTargetPos", m, sun_target);
  s.sun_object_dir = normalize3(sub3(obj_pos, obj_target), s.sun_dir);

  const auto haze_pos = p.vec3("HazeObjectPos", m, obj_pos);
  const auto haze_target = p.vec3("HazeObjectTargetPos", m, obj_target);
  s.haze_dir = normalize3(sub3(haze_pos, haze_target), s.sun_object_dir);

  s.ibl_ambient_scale = p.scalar("IBLAmbientScale", m, 1.0f);
  s.ibl_direct_env = p.scalar("IBLDirectScaleEnvironment", m, 1.3f);
  s.ibl_direct_car = p.scalar("IBLDirectScaleCar", m, 0.62f);

  const auto raw_sun = p.vec3("SunColor", m, {0.68f, 0.58f, 0.385f});
  const float sun_mult = p.scalar("SunColorMult", m, 1.0f);
  for (int i = 0; i < 3; ++i) {
    s.sun_color[i] = raw_sun[i] * sun_mult * s.ibl_direct_env;
    s.car_sun_color[i] = raw_sun[i] * sun_mult * s.ibl_direct_car;
  }
  s.sun_object_color = p.vec3("SunObjectColour", m, {1.0f, 0.99f, 0.92f});
  s.sun_object_size = p.scalar("SunObjectSize", m, 20.0f);
  s.sun_object_alpha = p.scalar("SunObjectAlpha", m, 0.5f);
  s.is_moon = p.scalar("SunObjectMoon", m, 0.0f) >= 0.5f;
  s.shadow_intensity = p.scalar("ShadowIntensity", m, 1.0f);

  s.hl_top = p.vec3("HLTopColour", m, {0.1687f, 0.2229f, 0.2746f});
  s.hl_bottom = p.vec3("HLBottomColour", m, {0.1788f, 0.1729f, 0.1024f});
  s.ambient_color = p.vec3("AmbientColour", m, {0.1717f, 0.2079f, 0.2229f});
  s.car_hl_top = p.vec3("CarHLColourTop", m, s.hl_top);
  s.car_hl_bottom = p.vec3("CarHLColourBottom", m, s.hl_bottom);

  // Atmosphere & stars.
  s.sky_gain = p.scalar("SkyGain", m, 2.0f);
  s.atmo_top = p.vec3("AtmosphereTopColour", m, {0.007f, 0.034f, 0.097f});
  s.atmo_bottom = p.vec3("AtmosphereBottomColour", m, {0.48f, 0.91f, 0.997f});
  s.atmo_color_power = p.scalar("AtmosphereColourPower", m, 0.165f);
  s.atmo_clamp_lo = p.scalar("AtmosphereBottomClampPower", m, 0.06f);
  s.atmo_clamp_hi = p.scalar("AtmosphereClampPower", m, 0.50f);
  s.atmo_haze_color = p.vec3("AtmosphereHazeColour", m, {0.246f, 0.472f, 0.497f});
  s.atmo_haze_power = p.scalar("AtmosphereHazePower", m, 5.0f);
  s.atmo_haze_intensity = p.scalar("AtmosphereHazeIntensity", m, 0.21f);
  s.atmo_haze_elliptical = p.scalar("AtmosphereHazeEllipticalScale", m, 1.0f);
  s.atmo_haze_alpha = p.scalar("AtmosphereHazeAlpha", m, 0.8f);
  s.star_fade = p.scalar("StarFade", m, 0.0f);

  // Far clouds.
  s.far_dense = p.vec3("FarCloudDenseColour", m, {0.39f, 0.39f, 0.44f});
  s.far_thin = p.vec3("FarCloudThinColour", m, {0.79f, 0.79f, 0.74f});
  s.far_thin_intensity = p.scalar("FarCloudThinIntensity", m, 1.5f);
  s.far_centre_alpha = p.scalar("FarCloudCentreAlpha", m, 1.0f);
  s.far_edge_alpha = p.scalar("FarCloudEdgeAlpha", m, 1.0f);
  s.far_alpha_power = p.scalar("FarCloudAlphaPower", m, 2.0f);
  s.far_coverage = p.scalar("FarCloudCoverage", m, 1.0f);
  s.far_back_dense = p.vec3("FarCloudBackDenseColour", m, s.far_dense);
  s.far_back_thin = p.vec3("FarCloudBackThinColour", m, s.far_thin);
  s.far_back_centre_alpha = p.scalar("FarCloudBackCentreAlpha", m, 1.0f);
  s.far_back_edge_alpha = p.scalar("FarCloudBackEdgeAlpha", m, 1.0f);
  s.far_back_coverage = p.scalar("FarCloudBackCoverage", m, 1.0f);
  s.far_back_speed_x = p.scalar("FarCloudBackSpeedX", m, -50.0f);
  s.far_back_speed_z = p.scalar("FarCloudBackSpeedZ", m, 40.0f);

  // Close lit clouds.
  s.close_top_color = p.vec3("CloseCloudTopColour", m, {1.0f, 0.985f, 0.888f});
  s.close_bottom_color = p.vec3("CloseCloudBottomColour", m, {0.84f, 0.29f, 0.001f});
  s.close_back_color = p.vec3("CloseCloudBackColour", m, {0.865f, 0.50f, 0.045f});
  s.close_dense_color = p.vec3("CloseCloudDenseColour", m, {0.063f, 0.113f, 0.136f});
  s.close_thin_color = p.vec3("CloseCloudThinColour", m, {0.35f, 0.544f, 0.742f});
  s.close_back_dense_color = p.vec3("CloseCloudDenseColourBack", m, s.close_dense_color);
  s.close_back_thin_color = p.vec3("CloseCloudThinColourBack", m, s.close_thin_color);
  s.close_back_bottom_color = p.vec3("CloseCloudBottomColourBack", m, s.close_bottom_color);
  s.close_dense_intensity = p.scalar("CloseCloudDenseColourIntensity", m, 1.0f);
  s.close_thin_intensity = p.scalar("CloseCloudThinColourIntensity", m, 1.0f);
  s.close_back_power = p.scalar("CloseCloudBackPower", m, 8.0f);
  s.close_back_intensity = p.scalar("CloseCloudBackIntensity", m, 0.25f);
  s.close_top_power = p.scalar("CloseCloudTopPower", m, 2.0f);
  s.close_top_intensity = p.scalar("CloseCloudTopIntensity", m, 0.5f);
  s.close_bottom_power = p.scalar("CloseCloudBottomPower", m, 2.0f);
  s.close_bottom_intensity = p.scalar("CloseCloudBottomIntensity", m, 0.05f);
  s.close_back_bottom_power = p.scalar("CloseCloudBottomPowerBack", m, s.close_bottom_power);
  s.close_back_bottom_intensity = p.scalar("CloseCloudBottomIntensityBack", m, s.close_bottom_intensity * 0.5f);
  s.close_top_mask_power = p.scalar("CloseCloudTopMaskPower", m, 1.0f);
  s.close_bottom_mask_power = p.scalar("CloseCloudBottomMaskPower", m, 1.5f);
  s.close_top_alpha = p.scalar("CloseCloudTopAlpha", m, 1.0f);
  s.close_bottom_alpha = p.scalar("CloseCloudBottomAlpha", m, 0.0f);
  s.close_front_alpha = p.scalar("CloseCloudFrontAlpha", m, 1.0f);
  s.close_back_alpha = p.scalar("CloseCloudBackAlpha", m, 0.0f);
  s.close_alpha_curve = p.scalar("CloseCloudAlphaCurve", m, 10.0f);
  s.close_front_revs = p.scalar("CloseCloudFrontRevs", m, 2.0f);
  s.close_back_revs = p.scalar("CloseCloudBackRevs", m, -1.269f);

  // Dynamic reflection cubemap sky & sun curves.
  s.cube_atmo_haze_color = p.vec3("CubeAtmosphereHazeColour", m, s.atmo_haze_color);
  s.cube_atmo_haze_power = p.scalar("CubeAtmosphereHazePower", m, s.atmo_haze_power);
  s.cube_atmo_haze_intensity = p.scalar("CubeAtmosphereHazeIntensity", m, s.atmo_haze_intensity);
  s.cube_atmo_haze_elliptical = p.scalar("CubeAtmosphereHazeEllipticalScale", m, s.atmo_haze_elliptical);
  s.cube_sun_object_color = p.vec3("CubeSunObjectColour", m, s.sun_object_color);
  s.cube_sun_object_size = p.scalar("CubeSunObjectSize", m, s.sun_object_size);

  // Fog.
  s.fog_color = p.vec3("FogColour", m, {0.091f, 0.1374f, 0.1446f});
  s.fog_density = p.scalar("FogDensity", m, 0.03f);
  s.fog_start = p.scalar("FogStartDistance", m, 975.0f);
  s.fog_sun_scatter_color = p.vec3("FogSunScatterColour", m, {0.223f, 0.223f, 0.223f});
  s.fog_sun_scatter_power = p.scalar("FogSunScatterPower", m, 5.0f);
  s.fog_sun_scatter_start = p.scalar("FogSunScatterStartDistance", m, 0.0f);
  s.fog_sun_scatter_rampup = p.scalar("FogSunScatterRampupDistance", m, 10000.0f);
  s.fog_height_fade_amount = p.scalar("FogHeightFadeAmount", m, 0.9f);
  s.fog_height_fade_start = p.scalar("FogHeightFadeStart", m, 200.0f);
  s.fog_height_fade_end = p.scalar("FogHeightFadeEnd", m, 4000.0f);
  s.fog_sky_height_fade_start = p.scalar("FogSkyHeightFadeStart", m, 0.0f);
  s.fog_sky_height_fade_end = p.scalar("FogSkyHeightFadeEnd", m, 1500.0f);

  // Clock, night lights & post multipliers.
  s.time_speed = p.scalar("TimeSpeed", m, 28.0f);
  s.switch_on_lights = clamp01(p.scalar("SwitchOnLights", m, 0.0f));
  s.day_night_post = clamp01(p.scalar("DayNightPostProcess", m, 0.0f));
  s.day_color_grade = clamp01(p.scalar("DayColorGradeAmount", m, 1.0f));
  s.night_color_grade = clamp01(p.scalar("NightColorGradeAmount", m, 0.0f));
  s.vignette_amount = p.scalar("VignetteAmount", m, 1.0f);
  s.light_rays_color = p.vec3("LightRaysColour", m, {1.0f, 0.9f, 0.7f});
  s.light_rays_intensity = p.scalar("LightRaysIntensity", m, 0.0f);
  s.bloom_scale_mult = p.scalar("BloomScaleMultiplier", m, 1.0f);
  s.bloom_cutoff_mult = p.scalar("BloomCutoffMultiplier", m, 1.0f);
  s.bloom_lower_grad_mult = p.scalar("BloomLowerGradientMultiplier", m, 1.0f);
  s.bloom_upper_grad_mult = p.scalar("BloomUpperGradientMultiplier", m, 1.0f);
  s.bloom_persist_mult = p.scalar("BloomPersistMultiplier", m, 1.0f);
  s.desat_amount_mult = p.scalar("DesatAmountMultiplier", m, 1.0f);
  s.desat_luminosity_mult = p.vec3("DesatLuminosityMultiplier", m, {1.0f, 1.0f, 1.0f});
  s.desat_post_mod_mult = p.vec3("DesatPostModMultiplier", m, {1.0f, 1.0f, 1.0f});
  s.contrast_mult = p.scalar("ContrastMultiplier", m, 1.0f);

  return s;
}

bool TrackPostConfig::load_xml(const std::string& xml_text) {
  XmlElem root;
  if (!parse_xml_root(xml_text, root)) return false;

  if (const auto* f = root.child_ci("filmicTone")) {
    white_day = f->num("white", white_day);
    white_night = f->num("whiteNight", white_night);
    filmic_exposure = f->num("exposure", filmic_exposure);
    const char* day_k[6] = {"shoulderStrength", "linearStrength", "linearAngle",
                            "toeStrength", "toeNumerator", "toeDenominator"};
    const char* night_k[6] = {"shoulderStrengthNight", "linearStrengthNight", "linearAngleNight",
                              "toeStrengthNight", "toeNumerator", "toeDenominator"};
    for (int i = 0; i < 6; ++i) {
      hable_day[i] = f->num(day_k[i], hable_day[i]);
      hable_night[i] = f->num(night_k[i], hable_night[i]);
    }
  }
  auto read_exposure = [](const XmlElem* e, float keys[4], float ev[4]) {
    if (!e) return;
    keys[0] = e->num("Key_ExtraDark", keys[0]);
    keys[1] = e->num("Key_Dark", keys[1]);
    keys[2] = e->num("Key_Light", keys[2]);
    keys[3] = e->num("Key_ExtraLight", keys[3]);
    ev[0] = e->num("Exp_ExtraDark", ev[0]);
    ev[1] = e->num("Exp_Dark", ev[1]);
    ev[2] = e->num("Exp_Light", ev[2]);
    ev[3] = e->num("Exp_ExtraLight", ev[3]);
  };
  read_exposure(root.child_ci("ToneMapAdaptiveExposure"), exterior_keys, exterior_ev);
  read_exposure(root.child_ci("ToneMapCockpitExposure"), cockpit_keys, cockpit_ev);
  if (const auto* d = root.child_ci("ToneMapeDelay")) tonemap_delay = d->num("val", tonemap_delay);
  if (const auto* r = root.child_ci("CameraExposureRange")) {
    exposure_min = r->num("minval", exposure_min);
    exposure_max = r->num("maxval", exposure_max);
  }
  if (const auto* d = root.child_ci("DesaturateAmount")) desat_amount = d->num("value", desat_amount);
  if (const auto* l = root.child_ci("DesatLuminosity")) {
    desat_luminosity[0] = l->num("r", desat_luminosity[0]);
    desat_luminosity[1] = l->num("g", desat_luminosity[1]);
    desat_luminosity[2] = l->num("b", desat_luminosity[2]);
  }
  if (const auto* pm = root.child_ci("DesatPostMod")) {
    desat_post_mod[0] = pm->num("r", desat_post_mod[0]);
    desat_post_mod[1] = pm->num("g", desat_post_mod[1]);
    desat_post_mod[2] = pm->num("b", desat_post_mod[2]);
  }
  if (const auto* eb = root.child_ci("ExposureBias")) exposure_bias = eb->num("value", exposure_bias);
  if (const auto* ct = root.child_ci("Contrast")) contrast = ct->num("value", contrast);
  if (const auto* br = root.child_ci("Brightness")) brightness = br->num("value", brightness);
  if (const auto* cs = root.child_ci("cubeSkyScale")) cube_sky_scale = cs->num("value", cube_sky_scale);
  if (const auto* csat = root.child_ci("cubeSkySaturation")) cube_sky_saturation = csat->num("value", cube_sky_saturation);
  if (const auto* cg = root.child_ci("cubeReflGroundScale")) cube_refl_ground_scale = cg->num("value", cube_refl_ground_scale);
  if (const auto* cm = root.child_ci("Cubemap")) cube_far_distance = cm->num("FarDistance", cube_far_distance);
  if (const auto* b = root.child_ci("bloom")) {
    if (const auto* c = b->child_ci("cutoff")) bloom_cutoff = c->num("value", bloom_cutoff);
    if (const auto* sc = b->child_ci("scale")) bloom_scale = sc->num("value", bloom_scale);
    if (const auto* ps = b->child_ci("persist")) bloom_persist = ps->num("value", bloom_persist);
    if (const auto* cv = b->child_ci("curve")) {
      bloom_lower_grad = cv->num("lowerGradient", bloom_lower_grad);
      bloom_upper_grad = cv->num("upperGradient", bloom_upper_grad);
    }
    if (const auto* w = b->child_ci("weights")) {
      const char* lk[5] = {"layer0", "layer1", "layer2", "layer3", "layer4"};
      for (int i = 0; i < 5; ++i) bloom_weights[i] = w->num(lk[i], bloom_weights[i]);
    }
  }
  if (const auto* sh = root.child_ci("ShadowEnds")) {
    shadow_near = sh->num("ShadowNear", shadow_near);
    shadow_far = sh->num("InGameShadowEnd", shadow_far);
  }
  if (const auto* sc = root.child_ci("ShadowColor")) {
    shadow_color[0] = sc->num("r", shadow_color[0]);
    shadow_color[1] = sc->num("g", shadow_color[1]);
    shadow_color[2] = sc->num("b", shadow_color[2]);
  }
  if (const auto* ss = root.child_ci("SunSize")) {
    sun_size_min = ss->num("min", sun_size_min);
    sun_size_max = ss->num("max", sun_size_max);
  }
  // Wind direction sanitization (SPEC-VFX-AND-PARTICLES: retail TrackSettings.xml
  // has an uninitialized z = -431602080.0; reject any |component| > 100).
  if (const auto* wd = root.child_ci("WindDirection")) {
    auto san = [](float v) { return (std::isfinite(v) && std::fabs(v) <= 100.0f) ? v : 0.0f; };
    float wx = san(wd->num("x", 1.0f));
    float wy = san(wd->num("y", 0.0f));
    float wz = san(wd->num("z", 0.0f));
    const float hlen = std::sqrt(wx * wx + wz * wz);
    if (hlen > 1e-5f) {
      wind_dir[0] = wx / hlen;
      wind_dir[1] = 0.0f;
      wind_dir[2] = wz / hlen;
    } else if (std::fabs(wy) > 1e-5f) {
      wind_dir[0] = 1.0f;
      wind_dir[1] = 0.0f;
      wind_dir[2] = 0.0f;
    }
  }
  if (const auto* wa = root.child_ci("WindAmplitude")) wind_amplitude = wa->num("value", wind_amplitude);
  if (const auto* ws = root.child_ci("WindSpeed")) wind_speed = ws->num("value", wind_speed);
  if (const auto* gh = root.child_ci("GrassHeightScale")) grass_height_scale = gh->num("value", grass_height_scale);
  if (const auto* gl = root.child_ci("GrassLODDist")) grass_lod_dist = gl->num("value", grass_lod_dist);
  if (const auto* gf = root.child_ci("GrassLODFadeRange")) grass_lod_fade_range = gf->num("value", grass_lod_fade_range);
  if (const auto* gm = root.child_ci("GrassMinNumShells")) grass_min_shells = int(gm->num("value", float(grass_min_shells)));
  if (const auto* gfs = root.child_ci("GrassFadeStartDistance")) grass_fade_start = gfs->num("value", grass_fade_start);
  if (const auto* gfe = root.child_ci("GrassFadeEndDistance")) grass_fade_end = gfe->num("value", grass_fade_end);
  if (const auto* mb = root.child_ci("motionBlurAmount")) motion_blur_amount = mb->num("value", motion_blur_amount);
  if (const auto* dbm = root.child_ci("DetailBlurMPHForMaxBlur")) detail_blur_mph_max = dbm->num("value", detail_blur_mph_max);
  if (const auto* dd = root.child_ci("dofFocalDistance")) dof_focal_distance = dd->num("value", dof_focal_distance);
  if (const auto* dl = root.child_ci("dofFocalLength")) dof_focal_length = dl->num("value", dof_focal_length);
  if (const auto* dmb = root.child_ci("dofFocalMaxBlur")) dof_focal_max_blur = dmb->num("value", dof_focal_max_blur);
  if (const auto* cr = root.child_ci("Crowds")) {
    crowd_dark[0] = cr->num("dark_x", crowd_dark[0]);
    crowd_dark[1] = cr->num("dark_y", crowd_dark[1]);
    crowd_dark[2] = cr->num("dark_z", crowd_dark[2]);
    crowd_light[0] = cr->num("light_x", crowd_light[0]);
    crowd_light[1] = cr->num("light_y", crowd_light[1]);
    crowd_light[2] = cr->num("light_z", crowd_light[2]);
    crowd_sprite_distance = cr->num("SpriteDistance", crowd_sprite_distance);
  }
  return true;
}

bool TrackPostConfig::load_file(const std::filesystem::path& path) {
  std::string text;
  return read_text_file(path, text) && load_xml(text);
}

float TrackPostConfig::motion_blur_strength(double speed_mps) const {
  if (!(motion_blur_amount > 0.0f)) return 0.0f;
  const double mph = std::max(0.0, speed_mps) * 2.23693629;
  const double mph_max = std::max(double(detail_blur_mph_max), 1.0);
  const double t = std::clamp(mph / mph_max, 0.0, 1.0);
  return float(std::clamp(t * double(motion_blur_amount), 0.0, 0.95));
}

float TrackPostConfig::evaluate_ev(float adapted_luminance, bool cockpit, float exposure_bias) const {
  const float* K = cockpit ? cockpit_keys : exterior_keys;
  const float* E = cockpit ? cockpit_ev : exterior_ev;
  float ev_curve = E[0];
  if (adapted_luminance <= K[0]) {
    ev_curve = E[0];
  } else if (adapted_luminance >= K[3]) {
    ev_curve = E[3];
  } else {
    for (int i = 0; i < 3; ++i) {
      if (adapted_luminance <= K[i + 1]) {
        const float span = K[i + 1] - K[i];
        const float t = span > 1e-6f ? clamp01((adapted_luminance - K[i]) / span) : 0.0f;
        ev_curve = E[i] + t * (E[i + 1] - E[i]);
        break;
      }
    }
  }
  return std::clamp(ev_curve + filmic_exposure + exposure_bias, exposure_min, exposure_max);
}

ColorLut16 ColorLut16::identity() {
  ColorLut16 lut;
  for (uint32_t b = 0; b < kSize; ++b) {
    for (uint32_t g = 0; g < kSize; ++g) {
      for (uint32_t r = 0; r < kSize; ++r) {
        const size_t idx = ((size_t(b) * kSize + g) * kSize + r) * 4;
        lut.rgba[idx + 0] = uint8_t(std::lround(double(r) * 255.0 / 15.0));
        lut.rgba[idx + 1] = uint8_t(std::lround(double(g) * 255.0 / 15.0));
        lut.rgba[idx + 2] = uint8_t(std::lround(double(b) * 255.0 / 15.0));
        lut.rgba[idx + 3] = 255;
      }
    }
  }
  return lut;
}

bool ColorLut16::load_dds(std::span<const uint8_t> bytes, std::string* error) {
  // Standard 128-byte DDS header + 16*16*16*4 = 16,512 bytes.
  if (bytes.size() < 128 + kByteSize || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
    if (error) *error = "invalid 16x16x16 DDS LUT header/size";
    return false;
  }
  auto le32 = [&](size_t off) -> uint32_t {
    return uint32_t(bytes[off]) | (uint32_t(bytes[off + 1]) << 8) |
           (uint32_t(bytes[off + 2]) << 16) | (uint32_t(bytes[off + 3]) << 24);
  };
  const uint32_t height = le32(12);
  const uint32_t width = le32(16);
  const uint32_t depth = le32(24);
  const uint32_t r_mask = le32(92);
  const uint32_t g_mask = le32(96);
  const uint32_t b_mask = le32(100);
  if (width != kSize || height != kSize || (depth != kSize && depth != 0)) {
    if (error) *error = "DDS LUT is not 16x16x16";
    return false;
  }
  const bool bgra = (r_mask == 0x00FF0000u && g_mask == 0x0000FF00u && b_mask == 0x000000FFu);
  const uint8_t* src = bytes.data() + 128;
  for (size_t i = 0; i < kSize * kSize * kSize; ++i) {
    const size_t o = i * 4;
    if (bgra) {
      rgba[o + 0] = src[o + 2];
      rgba[o + 1] = src[o + 1];
      rgba[o + 2] = src[o + 0];
      rgba[o + 3] = 255;
    } else {
      rgba[o + 0] = src[o + 0];
      rgba[o + 1] = src[o + 1];
      rgba[o + 2] = src[o + 2];
      rgba[o + 3] = 255;
    }
  }
  return true;
}

bool ColorLut16::load_dds_file(const std::filesystem::path& path, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!read_binary_file(path, bytes)) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  return load_dds(bytes, error);
}

ColorLut16 ColorLut16::blend(const ColorLut16& day_lut, const ColorLut16& night_lut,
                             float day_amt, float night_amt) {
  day_amt = clamp01(day_amt);
  night_amt = clamp01(night_amt);
  const float sum = day_amt + night_amt;
  if (sum > 1.0f) {
    day_amt /= sum;
    night_amt /= sum;
  }
  const float id_amt = 1.0f - (day_amt + night_amt);
  static const ColorLut16 kId = ColorLut16::identity();
  ColorLut16 out;
  for (size_t i = 0; i < kSize * kSize * kSize; ++i) {
    const size_t o = i * 4;
    for (int c = 0; c < 3; ++c) {
      const float v = id_amt * float(kId.rgba[o + c]) + day_amt * float(day_lut.rgba[o + c]) +
                      night_amt * float(night_lut.rgba[o + c]);
      out.rgba[o + c] = uint8_t(std::clamp(int(std::lround(v)), 0, 255));
    }
    out.rgba[o + 3] = 255;
  }
  return out;
}

std::array<float, 3> ColorLut16::sample(float r, float g, float b) const {
  const float fx = clamp01(r) * 15.0f;
  const float fy = clamp01(g) * 15.0f;
  const float fz = clamp01(b) * 15.0f;
  const uint32_t x0 = std::min<uint32_t>(uint32_t(fx), 14), x1 = x0 + 1;
  const uint32_t y0 = std::min<uint32_t>(uint32_t(fy), 14), y1 = y0 + 1;
  const uint32_t z0 = std::min<uint32_t>(uint32_t(fz), 14), z1 = z0 + 1;
  const float tx = fx - float(x0), ty = fy - float(y0), tz = fz - float(z0);
  auto at = [&](uint32_t x, uint32_t y, uint32_t z, int c) -> float {
    return float(rgba[((size_t(z) * kSize + y) * kSize + x) * 4 + size_t(c)]) / 255.0f;
  };
  std::array<float, 3> out{};
  for (int c = 0; c < 3; ++c) {
    const float c00 = at(x0, y0, z0, c) * (1.0f - tx) + at(x1, y0, z0, c) * tx;
    const float c10 = at(x0, y1, z0, c) * (1.0f - tx) + at(x1, y1, z0, c) * tx;
    const float c01 = at(x0, y0, z1, c) * (1.0f - tx) + at(x1, y0, z1, c) * tx;
    const float c11 = at(x0, y1, z1, c) * (1.0f - tx) + at(x1, y1, z1, c) * tx;
    const float c0 = c00 * (1.0f - ty) + c10 * ty;
    const float c1 = c01 * (1.0f - ty) + c11 * ty;
    out[size_t(c)] = c0 * (1.0f - tz) + c1 * tz;
  }
  return out;
}

void ColorLutBlender::snap(const ColorLut16& target) {
  initialized = true;
  current = target;
  for (size_t i = 0; i < ColorLut16::kByteSize; ++i) {
    cur_f[i] = float(target.rgba[i]);
  }
}

bool ColorLutBlender::update(const ColorLut16& target, float dt, float tau_sec, bool force_snap) {
  if (!initialized || force_snap || dt <= 0.0f) {
    snap(target);
    return true;
  }
  const float a = clamp01(1.0f - std::exp(-dt / std::max(tau_sec, 0.05f)));
  bool changed = false;
  for (size_t i = 0; i < ColorLut16::kSize * ColorLut16::kSize * ColorLut16::kSize; ++i) {
    const size_t o = i * 4;
    for (int c = 0; c < 3; ++c) {
      const float dst = float(target.rgba[o + size_t(c)]);
      cur_f[o + size_t(c)] += (dst - cur_f[o + size_t(c)]) * a;
      const uint8_t q = uint8_t(std::clamp(int(std::lround(cur_f[o + size_t(c)])), 0, 255));
      if (q != current.rgba[o + size_t(c)]) {
        current.rgba[o + size_t(c)] = q;
        changed = true;
      }
    }
    current.rgba[o + 3] = 255;
  }
  return changed;
}

bool ColorLutBlender::settled(const ColorLut16& target, float eps) const {
  if (!initialized) return false;
  for (size_t i = 0; i < ColorLut16::kSize * ColorLut16::kSize * ColorLut16::kSize; ++i) {
    const size_t o = i * 4;
    for (int c = 0; c < 3; ++c) {
      if (std::fabs(cur_f[o + size_t(c)] - float(target.rgba[o + size_t(c)])) > eps) return false;
    }
  }
  return true;
}

bool PostZone::contains(float px, float pz) const {
  auto sign = [](float x1, float z1, float x2, float z2, float x3, float z3) {
    return (x1 - x3) * (z2 - z3) - (x2 - x3) * (z1 - z3);
  };
  for (const auto& t : triangles) {
    const float d1 = sign(px, pz, t.x[0], t.z[0], t.x[1], t.z[1]);
    const float d2 = sign(px, pz, t.x[1], t.z[1], t.x[2], t.z[2]);
    const float d3 = sign(px, pz, t.x[2], t.z[2], t.x[0], t.z[0]);
    const bool has_neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    const bool has_pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    if (!(has_neg && has_pos)) return true;
  }
  return false;
}

bool PostZoneSet::load(const std::filesystem::path& world_dir, const std::filesystem::path& media_dir,
                       std::string* error) {
  zones.clear();
  luts.clear();
  bloom_templates.clear();

  // Fallback LUTs in track folder.
  fallback_day = ColorLut16::identity();
  fallback_night = ColorLut16::identity();
  fallback_day.load_dds_file(world_dir / "ColorGradingLookup.dds", nullptr);
  fallback_night.load_dds_file(world_dir / "ColorGradingLookup_Night.dds", nullptr);

  // Parse PostProcessingZones_Safe.xml.
  std::string zone_xml;
  const auto zones_path = world_dir / "Ribbon_00" / "PostProcessingZones_Safe.xml";
  if (read_text_file(zones_path, zone_xml)) {
    XmlElem root;
    if (parse_xml_root(zone_xml, root)) {
      for (const auto& z : root.children) {
        if (lower_ascii(z.tag) != "postprocessingzone") continue;
        PostZone pz;
        pz.name = z.attr("Name");
        pz.bloom_template = z.attr("BloomTemplate");
        pz.color_grading_day = lower_ascii(z.attr("ColorGradingMap"));
        pz.color_grading_night = lower_ascii(z.attr("ColorGradingMapNight"));
        pz.color_settings = z.attr("ColorSettings");
        pz.fog_template = z.attr("Fog");
        pz.vignette_top = z.attr("VignetteTop");
        pz.vignette_bottom = z.attr("VignetteBottom");
        pz.lerp_time = z.num("LerpTime", 15.0f);
        for (const auto& tri : z.children) {
          if (lower_ascii(tri.tag) != "postprocessingzonetriangle") continue;
          if (tri.children.size() < 3) continue;
          PostZone::Tri t;
          for (int k = 0; k < 3; ++k) {
            t.x[k] = tri.children[size_t(k)].num("posX", 0.0f);
            t.z[k] = tri.children[size_t(k)].num("posZ", 0.0f);
          }
          pz.triangles.push_back(t);
        }
        zones.push_back(std::move(pz));
      }
    }
  }

  // Load 22 3D LUTs and Bloom templates from media/dynamicpost.zip.
  const auto dyn_zip = media_dir / "dynamicpost.zip";
  if (std::filesystem::exists(dyn_zip)) {
    std::string aerr;
    if (auto arch = fh1::Archive::open(dyn_zip, &aerr)) {
      for (const auto& m : arch->members()) {
        const std::string low = lower_ascii(m.name);
        if (low.rfind("colourgradingmaps/", 0) == 0 && low.ends_with(".dds")) {
          const auto rd = arch->read(m);
          if (rd.ok()) {
            ColorLut16 lut;
            if (lut.load_dds(rd.data, nullptr)) {
              const std::string fname = low.substr(std::string("colourgradingmaps/").size());
              luts[fname] = lut;
            }
          }
        } else if (low == "tracks/colorado/bloom_templates.xml") {
          const auto rd = arch->read(m);
          if (rd.ok()) {
            std::string bxml(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
            XmlElem root;
            if (parse_xml_root(bxml, root)) {
              for (const auto& ch : root.children) {
                BloomTemplate bt;
                bt.cutoff = ch.num("Cutoff", bt.cutoff);
                bt.scale = ch.num("Scale", bt.scale);
                bt.persist = ch.num("Persist", bt.persist);
                bt.lower_grad = ch.num("LowerGradient", bt.lower_grad);
                bt.upper_grad = ch.num("UpperGradient", bt.upper_grad);
                const char* wk[5] = {"Weight0", "Weight1", "Weight2", "Weight3", "Weight4"};
                for (int i = 0; i < 5; ++i) bt.weights[i] = ch.num(wk[i], bt.weights[i]);
                const std::string nm = lower_ascii(ch.attr("Name", ch.tag));
                if (!nm.empty()) bloom_templates[nm] = bt;
              }
            }
          }
        } else if (low == "tracks/colorado/colorsettings_templates.xml") {
          const auto rd = arch->read(m);
          if (rd.ok()) {
            std::string cxml(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
            XmlElem root;
            if (parse_xml_root(cxml, root)) {
              for (const auto& ch : root.children) {
                ColorSettingsTemplate ct;
                ct.desat_amount = ch.num("DesatAmount", ct.desat_amount);
                ct.desat_luminosity[0] = ch.num("DesatLuminosityR", ct.desat_luminosity[0]);
                ct.desat_luminosity[1] = ch.num("DesatLuminosityG", ct.desat_luminosity[1]);
                ct.desat_luminosity[2] = ch.num("DesatLuminosityB", ct.desat_luminosity[2]);
                ct.desat_post_mod[0] = ch.num("DesatPostModR", ct.desat_post_mod[0]);
                ct.desat_post_mod[1] = ch.num("DesatPostModG", ct.desat_post_mod[1]);
                ct.desat_post_mod[2] = ch.num("DesatPostModB", ct.desat_post_mod[2]);
                ct.exposure_bias = ch.num("ExposureBias", ct.exposure_bias);
                ct.contrast = ch.num("Contrast", ct.contrast);
                ct.brightness = ch.num("Brightness", ct.brightness);
                const std::string nm = lower_ascii(ch.attr("Name", ch.tag));
                if (!nm.empty()) color_templates[nm] = ct;
              }
            }
          }
        } else if (low == "tracks/colorado/fog_templates.xml") {
          const auto rd = arch->read(m);
          if (rd.ok()) {
            std::string fxml(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
            XmlElem root;
            if (parse_xml_root(fxml, root)) {
              for (const auto& ch : root.children) {
                FogTemplate ft;
                ft.color[0] = ch.num("ColorR", ft.color[0]);
                ft.color[1] = ch.num("ColorG", ft.color[1]);
                ft.color[2] = ch.num("ColorB", ft.color[2]);
                ft.density = ch.num("Density", ft.density);
                ft.start_distance = ch.num("StartDistance", ft.start_distance);
                ft.sun_scatter_color[0] = ch.num("SunScatterColorR", ft.sun_scatter_color[0]);
                ft.sun_scatter_color[1] = ch.num("SunScatterColorG", ft.sun_scatter_color[1]);
                ft.sun_scatter_color[2] = ch.num("SunScatterColorB", ft.sun_scatter_color[2]);
                ft.sun_scatter_power = ch.num("SunScatterPower", ft.sun_scatter_power);
                ft.sun_scatter_start_distance = ch.num("SunScatterStartDistance", ft.sun_scatter_start_distance);
                ft.sun_scatter_rampup_distance = ch.num("SunScatterRampupDistance", ft.sun_scatter_rampup_distance);
                ft.height_fade_amount = ch.num("HeightFadeAmount", ft.height_fade_amount);
                ft.height_fade_start = ch.num("HeightFadeStart", ft.height_fade_start);
                ft.height_fade_end = ch.num("HeightFadeEnd", ft.height_fade_end);
                const std::string nm = lower_ascii(ch.attr("Name", ch.tag));
                if (!nm.empty()) fog_templates[nm] = ft;
              }
            }
          }
        } else if (low == "tracks/colorado/vignette_templates.xml") {
          const auto rd = arch->read(m);
          if (rd.ok()) {
            std::string vxml(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
            XmlElem root;
            if (parse_xml_root(vxml, root)) {
              for (const auto& ch : root.children) {
                VignetteTemplate vt;
                vt.angle_deg = ch.num("Angle", vt.angle_deg);
                vt.pos_x = ch.num("PosX", vt.pos_x);
                vt.pos_y = ch.num("PosY", vt.pos_y);
                vt.scale_x = ch.num("ScaleX", vt.scale_x);
                vt.scale_y = ch.num("ScaleY", vt.scale_y);
                vt.power = ch.num("Power", vt.power);
                vt.color[0] = ch.num("ColorR", vt.color[0]);
                vt.color[1] = ch.num("ColorG", vt.color[1]);
                vt.color[2] = ch.num("ColorB", vt.color[2]);
                vt.color[3] = ch.num("ColorA", vt.color[3]);
                vt.min_intensity = ch.num("MinIntensity", vt.min_intensity);
                vt.max_intensity = ch.num("MaxIntensity", vt.max_intensity);
                const std::string nm = lower_ascii(ch.attr("Name", ch.tag));
                if (!nm.empty()) vignette_templates[nm] = vt;
              }
            }
          }
        }
      }
    } else if (error) {
      *error = aerr;
    }
  }
  return !zones.empty() || !luts.empty();
}

int PostZoneSet::find_zone(float game_x, float game_z) const {
  for (size_t i = 0; i < zones.size(); ++i)
    if (zones[i].contains(game_x, game_z)) return int(i);
  return -1;
}

const ColorLut16& PostZoneSet::day_lut_for(int zone_index) const {
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = luts.find(zones[size_t(zone_index)].color_grading_day);
    if (it != luts.end()) return it->second;
  }
  return fallback_day;
}

const ColorLut16& PostZoneSet::night_lut_for(int zone_index) const {
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = luts.find(zones[size_t(zone_index)].color_grading_night);
    if (it != luts.end()) return it->second;
  }
  return fallback_night;
}

BloomTemplate PostZoneSet::bloom_for(int zone_index, const TrackPostConfig& defaults) const {
  BloomTemplate out;
  out.cutoff = defaults.bloom_cutoff;
  out.scale = defaults.bloom_scale;
  out.persist = defaults.bloom_persist;
  out.lower_grad = defaults.bloom_lower_grad;
  out.upper_grad = defaults.bloom_upper_grad;
  for (int i = 0; i < 5; ++i) out.weights[i] = defaults.bloom_weights[i];
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = bloom_templates.find(lower_ascii(zones[size_t(zone_index)].bloom_template));
    if (it != bloom_templates.end()) return it->second;
  }
  return out;
}

ColorSettingsTemplate PostZoneSet::color_settings_for(int zone_index, const TrackPostConfig& defaults) const {
  ColorSettingsTemplate out;
  out.desat_amount = defaults.desat_amount;
  out.exposure_bias = defaults.exposure_bias;
  out.contrast = defaults.contrast;
  out.brightness = defaults.brightness;
  for (int i = 0; i < 3; ++i) {
    out.desat_luminosity[i] = defaults.desat_luminosity[i];
    out.desat_post_mod[i] = defaults.desat_post_mod[i];
  }
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = color_templates.find(lower_ascii(zones[size_t(zone_index)].color_settings));
    if (it != color_templates.end()) return it->second;
  }
  return out;
}

FogTemplate PostZoneSet::fog_for(int zone_index) const {
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = fog_templates.find(lower_ascii(zones[size_t(zone_index)].fog_template));
    if (it != fog_templates.end()) return it->second;
  }
  return FogTemplate{};
}

VignetteTemplate PostZoneSet::vignette_top_for(int zone_index) const {
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = vignette_templates.find(lower_ascii(zones[size_t(zone_index)].vignette_top));
    if (it != vignette_templates.end()) return it->second;
  }
  return VignetteTemplate{};
}

VignetteTemplate PostZoneSet::vignette_bottom_for(int zone_index) const {
  if (zone_index >= 0 && size_t(zone_index) < zones.size()) {
    auto it = vignette_templates.find(lower_ascii(zones[size_t(zone_index)].vignette_bottom));
    if (it != vignette_templates.end()) return it->second;
  }
  return VignetteTemplate{};
}

void PostZoneBlender::update(const PostZoneSet& set, const TrackPostConfig& defaults, float game_x, float game_z,
                             float dt, bool snap) {
  const int z = set.find_zone(game_x, game_z);
  const auto tb = set.bloom_for(z, defaults);
  const auto tc = set.color_settings_for(z, defaults);
  const auto tf = set.fog_for(z);
  const auto tvt = set.vignette_top_for(z);
  const auto tvb = set.vignette_bottom_for(z);
  active_zone = z;
  if (!initialized || snap || dt <= 0.0f) {
    initialized = true;
    bloom = tb;
    color = tc;
    fog = tf;
    vig_top = tvt;
    vig_bottom = tvb;
    return;
  }
  const float lerp_s = (z >= 0 && size_t(z) < set.zones.size()) ? std::max(set.zones[size_t(z)].lerp_time, 1.0f) : 15.0f;
  const float a = std::clamp(1.0f - std::exp(-dt / (lerp_s * 0.35f)), 0.0f, 1.0f);
  auto mixf = [a](float& cur, float dst) { cur += (dst - cur) * a; };

  mixf(bloom.cutoff, tb.cutoff);
  mixf(bloom.scale, tb.scale);
  mixf(bloom.persist, tb.persist);
  mixf(bloom.lower_grad, tb.lower_grad);
  mixf(bloom.upper_grad, tb.upper_grad);
  for (int i = 0; i < 5; ++i) mixf(bloom.weights[i], tb.weights[i]);

  mixf(color.desat_amount, tc.desat_amount);
  mixf(color.exposure_bias, tc.exposure_bias);
  mixf(color.contrast, tc.contrast);
  mixf(color.brightness, tc.brightness);
  for (int i = 0; i < 3; ++i) {
    mixf(color.desat_luminosity[i], tc.desat_luminosity[i]);
    mixf(color.desat_post_mod[i], tc.desat_post_mod[i]);
    mixf(fog.color[i], tf.color[i]);
    mixf(fog.sun_scatter_color[i], tf.sun_scatter_color[i]);
  }
  mixf(fog.density, tf.density);
  mixf(fog.start_distance, tf.start_distance);
  mixf(fog.sun_scatter_power, tf.sun_scatter_power);
  mixf(fog.sun_scatter_start_distance, tf.sun_scatter_start_distance);
  mixf(fog.sun_scatter_rampup_distance, tf.sun_scatter_rampup_distance);
  mixf(fog.height_fade_amount, tf.height_fade_amount);
  mixf(fog.height_fade_start, tf.height_fade_start);
  mixf(fog.height_fade_end, tf.height_fade_end);

  auto mix_vig = [&](VignetteTemplate& v, const VignetteTemplate& tv) {
    mixf(v.angle_deg, tv.angle_deg);
    mixf(v.pos_x, tv.pos_x);
    mixf(v.pos_y, tv.pos_y);
    mixf(v.scale_x, tv.scale_x);
    mixf(v.scale_y, tv.scale_y);
    mixf(v.power, tv.power);
    for (int i = 0; i < 4; ++i) mixf(v.color[i], tv.color[i]);
    mixf(v.min_intensity, tv.min_intensity);
    mixf(v.max_intensity, tv.max_intensity);
  };
  mix_vig(vig_top, tvt);
  mix_vig(vig_bottom, tvb);
}

bool parse_stars_bin(std::span<const uint8_t> bytes, std::vector<Star>& out, std::string* error) {
  if (bytes.empty() || (bytes.size() % 16) != 0) {
    if (error) *error = "Stars.bin size is not a non-zero multiple of 16 bytes";
    return false;
  }
  const size_t count = bytes.size() / 16;
  out.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = bytes.data() + i * 16;
    out[i].x = read_be_f32(p + 0);
    out[i].y = read_be_f32(p + 4);
    out[i].z = read_be_f32(p + 8);
    out[i].brightness = read_be_f32(p + 12);
  }
  return true;
}

bool RealTimeSkyAssets::load_zip(const std::filesystem::path& zip_path, std::string* error) {
  auto arch = fh1::Archive::open(zip_path, error);
  if (!arch) return false;

  const auto stars_rd = arch->read("Stars.bin");
  if (!stars_rd.ok() || !parse_stars_bin(stars_rd.data, stars, error)) {
    if (error && error->empty()) *error = "realtimesky.zip: Stars.bin failed";
    return false;
  }

  if (!decode_sky_xds(*arch, "Sun.xds", sun, error)) return false;
  const char* moon_names[5] = {"Moon0.xds", "Moon1.xds", "Moon2.xds", "Moon3.xds", "Moon4.xds"};
  for (int i = 0; i < 5; ++i)
    if (!decode_sky_xds(*arch, moon_names[i], moons[size_t(i)], error)) return false;
  decode_sky_xds(*arch, "SkyDither.xds", dither, nullptr);

  // Far & close cloud layers (prefer full resolution, fall back to Small variants if needed).
  if (!decode_sky_xds(*arch, "FarCloudMapFront0.xds", far_cloud_front, error)) return false;
  if (!decode_sky_xds(*arch, "FarCloudMapBack0.xds", far_cloud_back, error)) return false;
  if (!decode_sky_xds(*arch, "FarCloudMask.xds", far_cloud_mask, error)) return false;
  if (!decode_sky_xds(*arch, "CloseCloudAlphaDensityFront0.xds", close_alpha_front, error)) return false;
  if (!decode_sky_xds(*arch, "CloseCloudAlphaDensityBack0.xds", close_alpha_back, error)) return false;
  if (!decode_sky_xds(*arch, "CloseCloudAmbientFront0.xds", close_ambient_front, error)) return false;
  if (!decode_sky_xds(*arch, "CloseCloudAmbientBack0.xds", close_ambient_back, error)) return false;
  if (!decode_sky_xds(*arch, "CloseCloudTopBottomAmbientFront0.xds", close_tb_front, error)) return false;
  if (!decode_sky_xds(*arch, "CloseCloudTopBottomAmbientBack0.xds", close_tb_back, error)) return false;

  return true;
}

bool TodSystem::load(const std::filesystem::path& world_dir, std::string* error) {
  loaded_tod = false;
  loaded_sky = false;

  post_config.load_file(world_dir / "TrackSettings.xml");

  const auto ribbon_dir = world_dir / "Ribbon_00";
  std::string err_a;
  if (preset_a.load_file(ribbon_dir / "TimeOfDayA.xml", &err_a)) {
    loaded_tod = true;
    if (preset_b.load_file(ribbon_dir / "TimeOfDayB.xml", nullptr)) {
      preset_b.merge_fallback(preset_a);
    } else {
      preset_b = preset_a;
    }
    if (preset_neutral.load_file(ribbon_dir / "TimeOfDayNeutral.xml", nullptr)) {
      preset_neutral.merge_fallback(preset_a);
    } else {
      preset_neutral = preset_a;
    }
  } else if (error) {
    *error = err_a;
  }

  // Locate media root (parent of tracks/<world>).
  std::filesystem::path media_dir;
  if (world_dir.has_parent_path() && world_dir.parent_path().has_parent_path()) {
    media_dir = world_dir.parent_path().parent_path();
  }
  zones.load(world_dir, media_dir, nullptr);

  if (!media_dir.empty()) {
    const auto sky_zip = media_dir / "realtimesky.zip";
    if (std::filesystem::exists(sky_zip)) {
      loaded_sky = sky_assets.load_zip(sky_zip, nullptr);
    }
  }
  return loaded_tod;
}

const TodPreset& TodSystem::active_preset(const TodClock& clock) const {
  return clock.use_preset_b ? preset_b : preset_a;
}

TodState TodSystem::evaluate(const TodClock& clock) const {
  TodState s{};
  if (loaded_tod) {
    s = evaluate_tod(active_preset(clock), clock.tod_sec);
  } else {
    s.tod_sec = float(clock.tod_sec);
    s.tod_min = float(clock.tod_sec / 60.0);
  }
  s.cube_sky_saturation = post_config.cube_sky_saturation;
  s.cube_refl_ground_scale = post_config.cube_refl_ground_scale;
  s.cube_far_distance = post_config.cube_far_distance;
  s.shadow_near = post_config.shadow_near;
  s.shadow_far = post_config.shadow_far;
  s.shadow_color = {post_config.shadow_color[0], post_config.shadow_color[1], post_config.shadow_color[2]};
  return s;
}

}  // namespace world
