#include "drive/lamps.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace drive {
namespace {

std::string lower_str(std::string_view s) {
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); ++i) out[i] = char(std::tolower(static_cast<unsigned char>(s[i])));
  return out;
}

uint32_t next_rng(uint32_t& s) {
  uint32_t x = s ? s : 0x7A31C95Du;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s = x;
  return x;
}

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

std::array<float, 3> scale3(std::array<float, 3> c, float k) { return {c[0] * k, c[1] * k, c[2] * k}; }
std::array<float, 3> lerp3(std::array<float, 3> a, std::array<float, 3> b, float t) {
  return {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])};
}

}  // namespace

bool CarLampState::tod_requests_headlamps(double tod_sec) {
  const double wrapped = std::fmod(std::fmod(tod_sec, 86400.0) + 86400.0, 86400.0);
  return wrapped < kMorningCutoffSec || wrapped > kEveningStartSec;
}

void CarLampState::update(const LampInputs& in) {
  // 2.1 Brake lamps: strictly > 0 on BOTH brake_cmd and brake_pedal (handbrake ignored).
  const bool raw_brake = (in.brake_cmd > 0.0f && in.brake_pedal > 0.0f);
  if (in.is_ai) {
    if (raw_brake && !brake_on) ai_brake_hold = kAiBrakeHoldTicks;
    if (raw_brake) {
      brake_on = true;
    } else if (ai_brake_hold > 0) {
      --ai_brake_hold;
      brake_on = true;
    } else {
      brake_on = false;
    }
  } else {
    brake_on = raw_brake;
    ai_brake_hold = 0;
  }

  // 2.2 Reverse lamps: active iff gear == 0.
  reverse_on = (in.gear == 0);

  // 2.3 Turn indicators and hazards (0.36 s on, 0.33 s off).
  if (in.indicator_mode == 0) {
    ind_timer = 0.0f;
    ind_left = 0.0f;
    ind_right = 0.0f;
    left_ind_active = false;
    right_ind_active = false;
  } else {
    ind_timer += std::max(in.dt, 0.0f);
    const float period = kIndicatorOnSec + kIndicatorOffSec;
    if (ind_timer > period) ind_timer = std::fmod(ind_timer, period);
    const float phase = ind_timer < kIndicatorOnSec ? 1.0f : 0.0f;
    left_ind_active = (in.indicator_mode == 1 || in.indicator_mode == 3);
    right_ind_active = (in.indicator_mode == 2 || in.indicator_mode == 3);
    ind_left = left_ind_active ? phase : 0.0f;
    ind_right = right_ind_active ? phase : 0.0f;
  }

  // 2.4 & 2.5 Headlamps, full beam, fog lights, DRLs.
  bool req_head = false;
  fullbeam_on = false;
  fog_on = false;
  if (in.light_override == 0) {
    req_head = tod_requests_headlamps(in.tod_sec);
  } else if (in.light_override == 1) {
    req_head = true;
  } else if (in.light_override == 2) {
    req_head = true;
    fullbeam_on = true;
  } else if (in.light_override == 3) {
    req_head = true;
    fog_on = true;
  }

  if (req_head != headlamps_on) {
    if (!in.is_ai || in.snap_headlamps) {
      headlamps_on = req_head;
    } else {
      const float u = float(next_rng(rng_state)) / 4294967296.0f;
      if (u < 0.05f) headlamps_on = req_head;
    }
  }
  if (in.popup_open < 1.0f) {
    headlamps_on = false;
    fullbeam_on = false;
  }
  drl_on = true;
}

LampShaderVectors CarLampState::vectors() const {
  LampShaderVectors v;
  v.dynamic_lights_amount[0] = brake_on ? 1.0f : 0.0f;
  v.dynamic_lights_amount[1] = reverse_on ? 1.0f : 0.0f;
  v.dynamic_lights_amount[2] = ind_left;
  v.dynamic_lights_amount[3] = ind_right;

  v.dynamic_lights_2[0] = headlamps_on ? 1.0f : 0.0f;
  v.dynamic_lights_2[1] = fullbeam_on ? 1.0f : 0.0f;
  v.dynamic_lights_2[2] = drl_on ? 1.0f : 0.0f;
  v.dynamic_lights_2[3] = fog_on ? 1.0f : 0.0f;

  v.dynamic_lights_3[0] = left_ind_active ? 1.0f : 0.0f;
  v.dynamic_lights_3[1] = right_ind_active ? 1.0f : 0.0f;
  v.dynamic_lights_3[2] = headlamps_on ? 1.0f : 0.0f;
  v.dynamic_lights_3[3] = 0.0f;
  return v;
}

float CarLampState::point_active(const fh1::CarbinLampPoint& pt) const {
  if ((pt.side_flags & 0x80u) != 0u) return 1.0f;
  float a = 0.0f;
  if ((pt.channel_mask & 0x01u) && headlamps_on) a = std::max(a, fullbeam_on ? 1.25f : 1.0f);
  if ((pt.channel_mask & 0x02u) && headlamps_on) a = std::max(a, 0.85f);
  if ((pt.channel_mask & 0x04u) && brake_on) a = std::max(a, 1.0f);
  if (pt.channel_mask & 0x08u) {
    if ((pt.side_flags & 0x01u) && ind_left > 0.0f) a = std::max(a, ind_left);
    if ((pt.side_flags & 0x02u) && ind_right > 0.0f) a = std::max(a, ind_right);
  }
  if ((pt.channel_mask & 0x10u) && reverse_on) a = std::max(a, 1.0f);
  return a;
}

LampChannel classify_lamp_technique(std::string_view technique) {
  const std::string t = lower_str(technique);
  // Passive covers and reflectors MUST NEVER be made emissive (CAR_LAMPS.md §3.2).
  if (t == "detail_glass_red" || t == "detail_glass_clear" || t == "detail_glass_amber" ||
      t == "lights_glass" || t == "light_glass" || t == "lights_gls_noemit" || t == "red_glass" ||
      t == "head_light" || t == "head_light2" || t == "reflector" || t == "textured_reflector") {
    return LampChannel::kNone;
  }

  // Brake lamps.
  if (t == "tail_light" || t == "tail_light_lod0" || t == "tail_light_s_lod0" ||
      t == "tail_light_on" || t == "tail_light2" || t == "lights_gls_tail_light" || t == "white_brake") {
    return LampChannel::kBrake;
  }
  // Running tail lamps.
  if (t == "taillightst" || t == "lights_gls_taillightst") return LampChannel::kTail;
  // Two-stage tail/brake.
  if (t == "taillight2s" || t == "lights_gls_taillight2s") return LampChannel::kTailBrake2S;
  // Reverse.
  if (t == "reverse_light" || t == "reverse_light_lod0" || t == "lights_gls_reverse_light")
    return LampChannel::kReverse;
  if (t == "brkreverse") return LampChannel::kBrakeReverse;
  // Indicators.
  if (t == "indicator_left" || t == "lights_gls_indicator_left" || t == "linred")
    return LampChannel::kIndicatorLeft;
  if (t == "indicator_right" || t == "lights_gls_indicator_right" || t == "rinred")
    return LampChannel::kIndicatorRight;
  if (t == "brinl") return LampChannel::kBrakeIndLeft;
  if (t == "brinr") return LampChannel::kBrakeIndRight;
  if (t == "lindtailside") return LampChannel::kTailBrakeIndLeft;
  if (t == "rindtailside") return LampChannel::kTailBrakeIndRight;
  // Headlamps / high beam / fog.
  if (t == "oldhead" || t == "lights_gls_oldhead") return LampChannel::kHeadOld;
  if (t == "xenonhead" || t == "lights_gls_xenonhead") return LampChannel::kHeadXenon;
  if (t == "hidhead" || t == "lights_gls_hidhead") return LampChannel::kHeadHid;
  if (t == "fullbeam" || t == "lights_gls_fullbeam") return LampChannel::kFullBeam;
  if (t == "fogwhite" || t == "lights_gls_fogwhite") return LampChannel::kFogWhite;
  if (t == "fogred" || t == "lights_gls_fogred") return LampChannel::kFogRed;
  // DRLs.
  if (t == "drlwhiteswitch") return LampChannel::kDrlWhiteSwitch;
  if (t == "drlwhite") return LampChannel::kDrlWhite;
  if (t == "drlred") return LampChannel::kDrlRed;
  if (t == "drlorange") return LampChannel::kDrlOrange;
  if (t == "drlbrakered") return LampChannel::kDrlBrakeRed;
  if (t == "drlbrakeorange") return LampChannel::kDrlBrakeOrange;
  if (t.rfind("drlinl", 0) == 0) return LampChannel::kIndicatorLeft;
  if (t.rfind("drlinr", 0) == 0) return LampChannel::kIndicatorRight;
  // Side markers & number plate.
  if (t == "slorange") return LampChannel::kSideLightOrange;
  if (t == "slinlorange") return LampChannel::kSideIndLeft;
  if (t == "slinrorange") return LampChannel::kSideIndRight;
  if (t == "numplate") return LampChannel::kNumPlate;
  // Cockpit backlit gauges & digital displays.
  if (t == "gauge_emissive" || t == "z_gauge_emissive_alpha" || t == "gauge_emissive_alpha" ||
      t == "gauge_needle" || t == "cockpit_gauge_needle" || t.rfind("cockpit_gauge_emissive", 0) == 0) {
    return LampChannel::kGaugeBacklight;
  }
  if (t == "digital_gauge" || t == "cockpit_digital_gauge") return LampChannel::kDigitalGauge;
  return LampChannel::kNone;
}

void LampColorTable::load_shader_settings_xml(std::string_view xml_text) {
  auto find_rgb = [&](std::string_view key, std::array<float, 3>& dst) {
    const std::string low = lower_str(xml_text);
    const std::string needle = "<" + lower_str(key);
    const size_t pos = low.find(needle);
    if (pos == std::string::npos) return;
    const size_t end = low.find('>', pos);
    if (end == std::string::npos) return;
    const std::string tag = low.substr(pos, end - pos);
    auto read_attr = [&](const char* attr, float fallback) -> float {
      const std::string a = std::string(attr) + "=\"";
      const size_t p = tag.find(a);
      if (p == std::string::npos) return fallback;
      return std::strtof(tag.c_str() + p + a.size(), nullptr);
    };
    if (tag.find("r=\"") != std::string::npos) {
      dst[0] = read_attr("r", dst[0]);
      dst[1] = read_attr("g", dst[1]);
      dst[2] = read_attr("b", dst[2]);
    } else if (tag.find("x=\"") != std::string::npos) {
      dst[0] = read_attr("x", dst[0]);
      dst[1] = read_attr("y", dst[1]);
      dst[2] = read_attr("z", dst[2]);
    }
  };
  find_rgb("BrakeColor", brake);
  find_rgb("TailLightColor", tail);
  find_rgb("ReverseLightColor", reverse);
  find_rgb("IndicatorLightColor", indicator_amber);
  find_rgb("IndicatorRedLightColor", indicator_red);
  find_rgb("OldHeadlightColor", head_old);
  find_rgb("XenonHeadlightColor", head_xenon);
  find_rgb("HidHeadlightColor", head_hid);
  find_rgb("FullbeamColor", fullbeam);
  find_rgb("FogWhiteColor", fog_white);
  find_rgb("FogRedColor", fog_red);
  find_rgb("DRLWhiteColor", drl_white);
  find_rgb("DRLRedColor", drl_red);
  find_rgb("DRLOrangeColor", drl_orange);
  find_rgb("SideLightsOrangeColor", side_orange);
  find_rgb("NumberPlateLightColor", numplate);
}

std::array<float, 3> evaluate_lamp_emission(LampChannel ch, const LampShaderVectors& v,
                                            const LampColorTable& c) {
  const float brake = v.dynamic_lights_amount[0];
  const float rev = v.dynamic_lights_amount[1];
  const float ind_l = v.dynamic_lights_amount[2];
  const float ind_r = v.dynamic_lights_amount[3];
  const float head = v.dynamic_lights_2[0];
  const float full = v.dynamic_lights_2[1];
  const float drl = v.dynamic_lights_2[2];
  const float fog = v.dynamic_lights_2[3];
  const float l_act = v.dynamic_lights_3[0];
  const float r_act = v.dynamic_lights_3[1];
  const float side = v.dynamic_lights_3[2];

  switch (ch) {
    case LampChannel::kNone: return {0, 0, 0};
    case LampChannel::kBrake: return scale3(c.brake, brake);
    case LampChannel::kTail: return scale3(c.tail, head);
    case LampChannel::kTailBrake2S:
      return scale3(lerp3(c.tail, c.brake, brake), clamp01(head + brake));
    case LampChannel::kReverse: return scale3(c.reverse, rev);
    case LampChannel::kBrakeReverse:
      return scale3(lerp3(c.brake, c.reverse, rev), clamp01(brake + rev));
    case LampChannel::kIndicatorLeft: return scale3(c.indicator_amber, ind_l);
    case LampChannel::kIndicatorRight: return scale3(c.indicator_amber, ind_r);
    case LampChannel::kBrakeIndLeft: return scale3(c.brake, l_act > 0.5f ? ind_l : brake);
    case LampChannel::kBrakeIndRight: return scale3(c.brake, r_act > 0.5f ? ind_r : brake);
    case LampChannel::kTailBrakeIndLeft: {
      const float amt = l_act > 0.5f ? ind_l : clamp01(head + brake);
      const auto col = l_act > 0.5f ? c.indicator_red : lerp3(c.tail, c.brake, brake);
      return scale3(col, amt);
    }
    case LampChannel::kTailBrakeIndRight: {
      const float amt = r_act > 0.5f ? ind_r : clamp01(head + brake);
      const auto col = r_act > 0.5f ? c.indicator_red : lerp3(c.tail, c.brake, brake);
      return scale3(col, amt);
    }
    case LampChannel::kHeadOld: return scale3(c.head_old, head);
    case LampChannel::kHeadXenon: return scale3(c.head_xenon, head);
    case LampChannel::kHeadHid: return scale3(c.head_hid, head);
    case LampChannel::kFullBeam: return scale3(c.fullbeam, full);
    case LampChannel::kFogWhite: return scale3(c.fog_white, fog);
    case LampChannel::kFogRed: return scale3(c.fog_red, fog);
    case LampChannel::kDrlWhite: return scale3(c.drl_white, drl);
    case LampChannel::kDrlRed: return scale3(c.drl_red, drl);
    case LampChannel::kDrlOrange: return scale3(c.drl_orange, drl);
    case LampChannel::kDrlWhiteSwitch: return scale3(c.drl_white, drl * (1.0f - head));
    case LampChannel::kDrlBrakeRed: return scale3(c.drl_red, std::max(0.35f * drl, brake));
    case LampChannel::kDrlBrakeOrange: return scale3(c.drl_orange, std::max(0.35f * drl, brake));
    case LampChannel::kSideLightOrange: return scale3(c.side_orange, side);
    case LampChannel::kSideIndLeft: return scale3(c.side_orange, std::max(side * 0.5f, ind_l));
    case LampChannel::kSideIndRight: return scale3(c.side_orange, std::max(side * 0.5f, ind_r));
    case LampChannel::kNumPlate: return scale3(c.numplate, head);
    case LampChannel::kGaugeBacklight: return {c.gauge_emissive * head, c.gauge_emissive * head, c.gauge_emissive * head};
    case LampChannel::kDigitalGauge: return {0.45f, 0.45f, 0.45f};
  }
  return {0, 0, 0};
}

HeadlightBeamSpec resolve_headlight_beam(std::span<const fh1::CarbinLampPoint> points, bool fullbeam) {
  HeadlightBeamSpec out;
  out.range_m = fullbeam ? 95.0f : 55.0f;
  float sum[3] = {0, 0, 0};
  int count = 0;
  int best_prio = 0;
  uint8_t best_kind = 0;
  // Priority order from CAR_LAMPS.md §5: kind 2 xenon > kind 1 halogen > kind 3 HID > default.
  auto kind_prio = [](uint8_t k) -> int {
    if (k == 2) return 3;
    if (k == 1) return 2;
    if (k == 3) return 1;
    return 0;
  };
  for (const auto& pt : points) {
    if ((pt.channel_mask & 0x01u) == 0u) continue;
    // Some cars tag rear/side markers with bit 0x01; only average forward front headlamps.
    if (pt.position[2] < 0.5f) continue;
    for (int i = 0; i < 3; ++i) sum[i] += pt.position[size_t(i)];
    ++count;
    const int pr = kind_prio(pt.lamp_kind);
    if (pr > best_prio) {
      best_prio = pr;
      best_kind = pt.lamp_kind;
    }
  }
  if (count > 0) {
    out.valid = true;
    for (int i = 0; i < 3; ++i) out.origin_body[size_t(i)] = sum[i] / float(count);
  } else {
    out.valid = true;  // fallback nose origin
  }
  if (best_kind == 2) out.color = {0.60f, 0.70f, 1.20f};
  else if (best_kind == 1) out.color = {1.00f, 1.00f, 0.80f};
  else if (best_kind == 3) out.color = {1.00f, 0.85f, 0.40f};
  else out.color = {1.00f, 1.00f, 1.00f};
  return out;
}

ResolvedLampGlow resolve_lamp_glow(const fh1::CarbinLampPoint& pt) {
  ResolvedLampGlow out;
  const bool has_rgb = (pt.rgb[0] | pt.rgb[1] | pt.rgb[2]) != 0;
  if (has_rgb) {
    out.color = {float(pt.rgb[0]) / 255.0f, float(pt.rgb[1]) / 255.0f, float(pt.rgb[2]) / 255.0f};
  } else if (pt.channel_mask & 0x01u) {
    if (pt.lamp_kind == 2) out.color = {0.75f, 0.85f, 1.0f};
    else out.color = {1.0f, 0.98f, 0.92f};
  } else if (pt.channel_mask & (0x02u | 0x04u)) {
    out.color = {1.0f, 0.08f, 0.04f};
  } else if (pt.channel_mask & 0x08u) {
    out.color = {1.0f, 0.55f, 0.0f};
  } else {
    out.color = {1.0f, 1.0f, 1.0f};
  }
  out.radius_m = pt.size_raw > 0 ? float(pt.size_raw) / 128.0f : 0.35f;
  out.texture_index = (pt.texture_index >= 1 && pt.texture_index <= 7) ? int(pt.texture_index) : 1;
  out.inner_cone_deg = pt.inner_cone_deg == 255 ? 30.0f : float(pt.inner_cone_deg);
  out.outer_cone_deg = pt.outer_cone_deg == 255 ? 65.0f : float(pt.outer_cone_deg);
  if (out.outer_cone_deg <= out.inner_cone_deg + 1.0f) out.outer_cone_deg = out.inner_cone_deg + 15.0f;
  return out;
}

void CockpitAnimState::update_steering(float road_wheel_angle_rad, float speed_mps, float dt, float kick_rad) {
  const float speed_t = clamp01(std::fabs(speed_mps) / 50.0f);
  const float mult = 12.0f + speed_t * (6.0f - 12.0f);
  const float target = std::clamp(road_wheel_angle_rad * mult * kSteeringWheelAngleScale, -4.712389f, 4.712389f);
  const float max_rate = 720.0f * (3.14159265f / 180.0f);  // 720 deg/s slew limit
  const float gain = 14.0f;
  const float step = std::clamp((target - wheel_rad) * gain * dt, -max_rate * dt, max_rate * dt);
  wheel_rad = std::clamp(wheel_rad + step + kick_rad, -4.712389f, 4.712389f);
}

float CockpitAnimState::eval_gauge_norm(float value, float min_val, float max_val,
                                        std::span<const std::pair<float, float>> cal_points) {
  if (cal_points.empty()) {
    const float span = max_val - min_val;
    return span > 1e-6f ? clamp01((value - min_val) / span) : 0.0f;
  }
  float p0 = 0.0f, v0 = min_val;
  for (const auto& [pos, val] : cal_points) {
    if (value <= val) {
      const float span = val - v0;
      const float t = span > 1e-6f ? clamp01((value - v0) / span) : 0.0f;
      return std::clamp(p0 + t * (pos - p0), -1.0f, 1.0f);
    }
    p0 = pos;
    v0 = val;
  }
  const float span = max_val - v0;
  const float t = span > 1e-6f ? clamp01((value - v0) / span) : 0.0f;
  return std::clamp(p0 + t * (1.0f - p0), -1.0f, 1.0f);
}

void CockpitAnimState::update_gauges(float speed_mps, float max_speed_mps, float rpm, float max_rpm,
                                     float boost_psi, float max_boost_psi, float fuel_frac) {
  speed_norm = eval_gauge_norm(std::max(speed_mps, 0.0f), 0.0f, std::max(max_speed_mps, 10.0f));
  tach_norm = eval_gauge_norm(std::max(rpm, 0.0f), 0.0f, std::max(max_rpm, 1000.0f));
  fuel_norm = clamp01(fuel_frac);
  boost_norm = eval_gauge_norm(boost_psi, -14.7f, std::max(max_boost_psi, 5.0f));
}

}  // namespace drive
