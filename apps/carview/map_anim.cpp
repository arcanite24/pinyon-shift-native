#include "map_anim.h"

#include <algorithm>
#include <cmath>

#include "audio/xml.h"

namespace carview::mapanim {

bool channel_of(std::string_view type, Channel& out) {
  struct Name {
    std::string_view name;
    Channel ch;
  };
  static constexpr Name kNames[] = {
      {"posX", kPosX},     {"posY", kPosY},     {"scaleX", kScaleX}, {"posX2", kScaleX}, {"scaleY", kScaleY},
      {"zoom", kScaleY},   {"alpha", kAlpha},   {"angle", kAngle},   {"posY2", kAngle},  {"colour", kColour},
  };
  for (const auto& n : kNames)
    if (audio::iequals(type, n.name)) {
      out = n.ch;
      return true;
    }
  return false;
}

Ease ease_of(std::string_view name, bool present) {
  if (!present) return kBoth;
  if (audio::iequals(name, "in")) return kIn;
  if (audio::iequals(name, "out")) return kOut;
  if (audio::iequals(name, "both")) return kBoth;
  if (audio::iequals(name, "none")) return kNone;
  return kSmooth;  // "smooth", and anything else
}

Loop loop_of(std::string_view name, Loop fallback) {
  if (name.empty() || audio::iequals(name, "stop")) return kStop;
  if (audio::iequals(name, "loop")) return kLoop;
  if (audio::iequals(name, "pingpong")) return kPingPong;
  return fallback;
}

Animation parse_animation(const audio::XmlNode& node) {
  Animation a;
  a.name = node.attr("name");
  a.speed = float(node.number("speed", 1.0));
  a.loop = loop_of(node.attr("loop"), kStop);
  float t = 0;
  for (const auto& frame : node.children) {
    if (!audio::iequals(frame.name, "frame")) continue;
    t += float(frame.number("advance", 0.0));
    for (const auto& k : frame.children) {
      if (audio::iequals(k.name, "key")) {
        Channel c;
        if (!channel_of(k.attr("type"), c)) continue;
        a.channels[c].push_back({t, float(k.number("value", 0.0)), ease_of(k.attr("ease"), k.has("ease"))});
      } else if (audio::iequals(k.name, "trigger")) {
        a.triggers.push_back({t, k.attr("type"), k.attr("value")});
      }
    }
  }
  // The key times scale by the speed, and the length is the last frame's.
  for (auto& ch : a.channels)
    for (auto& k : ch) k.t *= a.speed;
  for (auto& tr : a.triggers) tr.t *= a.speed;
  a.duration = t * a.speed;
  return a;
}

float Animation::sample(Channel c, float t, float fallback) const {
  const auto& keys = channels[c];
  if (keys.empty()) return fallback;
  if (t >= keys.back().t) return keys.back().v;
  if (t <= keys.front().t) return keys.front().v;
  size_t bi = 1;
  while (bi < keys.size() && !(keys[bi].t > t)) ++bi;
  const Key& a = keys[bi - 1];
  const Key& b = keys[bi];
  const float span = b.t - a.t;
  const float s = span > 0 ? (t - a.t) / span : 1.0f;
  // The phantom values before a and after b set the tangents (Catmull-Rom).
  float p0, p3;
  switch (a.ease) {
    case kNone:
    case kIn: p0 = 2 * a.v - b.v; break;
    case kSmooth: p0 = bi >= 2 ? keys[bi - 2].v : b.v; break;
    default: p0 = b.v; break;  // kOut, kBoth: flat leaving a
  }
  switch (b.ease) {
    case kNone:
    case kOut: p3 = 2 * b.v - a.v; break;
    case kSmooth: p3 = bi + 1 < keys.size() ? keys[bi + 1].v : a.v; break;
    default: p3 = a.v; break;  // kIn, kBoth: flat arriving at b
  }
  const float s2 = s * s, s3 = s2 * s;
  return 0.5f * ((-s3 + 2 * s2 - s) * p0 + (3 * s3 - 5 * s2 + 2) * a.v + (-3 * s3 + 4 * s2 + s) * b.v + (s3 - s2) * p3);
}

float Animation::wrap(float elapsed) const {
  if (duration <= 0) return elapsed;
  switch (loop) {
    case kLoop: return std::fmod(std::max(0.0f, elapsed), duration);
    case kPingPong: {
      float t = std::fmod(std::max(0.0f, elapsed), 2 * duration);
      if (t >= duration) t = duration - (t - duration);
      return t;
    }
    default: return elapsed;
  }
}

std::vector<const Trigger*> Animation::crossed(float t0, float t1) const {
  std::vector<const Trigger*> out;
  if (!(t1 > t0)) return out;
  if (loop == kLoop && duration > 0) {
    const int c0 = int(std::floor(std::max(0.0f, t0) / duration)), c1 = int(std::floor(t1 / duration));
    for (int c = c0; c <= c1; ++c)
      for (const auto& tr : triggers) {
        const float at = tr.t + float(c) * duration;
        if (at > t0 && at <= t1) out.push_back(&tr);
      }
    return out;
  }
  for (const auto& tr : triggers)
    if (tr.t > t0 && tr.t <= t1) out.push_back(&tr);
  return out;
}

const Animation* Library::find(std::string_view name) const {
  const auto it = by_name.find(name);
  return it == by_name.end() ? nullptr : &it->second;
}

void parse_animations(const audio::XmlNode& animations, Library& out) {
  for (const auto& n : animations.children) {
    if (!audio::iequals(n.name, "animation")) continue;
    Animation a = parse_animation(n);
    if (a.name.empty()) continue;
    out.by_name.emplace(a.name, std::move(a));  // the first of a name stays
  }
}

Bindings parse_bindings(const audio::XmlNode& symbolizer) {
  Bindings out;
  for (const auto& n : symbolizer.children) {
    if (!audio::iequals(n.name, "animation")) continue;
    const std::string ev = n.attr("event"), anim = n.attr("animation");
    if (!ev.empty() && !anim.empty()) out.emplace(ev, anim);
  }
  return out;
}

void apply(const Animation& a, float elapsed, IconLook& look) {
  const float t = a.wrap(elapsed);
  look.scale_x = a.sample(kScaleX, t, look.scale_x);
  look.scale_y = a.sample(kScaleY, t, look.scale_y);
  look.alpha = a.sample(kAlpha, t, look.alpha);
  look.angle_deg = a.sample(kAngle, t, look.angle_deg);
}

CameraMove camera_at(const Animation& a, float elapsed) {
  CameraMove m;
  const float t = a.wrap(elapsed);
  m.has_target = a.has(kPosX) && a.has(kPosY);
  m.has_zoom = a.has(kScaleY);
  m.x = a.sample(kPosX, t, 0);
  m.z = a.sample(kPosY, t, 0);
  m.zoom = a.sample(kScaleY, t, 0);
  m.weight = std::clamp(a.sample(kAlpha, t, 1), 0.0f, 1.0f);
  return m;
}

}  // namespace carview::mapanim
