// The map render profiles' animations (MapProfileFullscreen.xml
// <animations>, docs/UI_SCENE_ANIMATION.md "Map profile animations"): the
// icons' event animations (intro
// grow, hover bulge, ghost, the player's radar ping, the career reveal drops)
// and the camera's unlock moves (the barn rumour reveal). Plain data and
// maths, no renderer: map_view.cpp draws the icons with them and the drive
// session moves the map's camera with them.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace audio {
struct XmlNode;
}

namespace carview::mapanim {

// A key's channel. The title keeps seven value tracks per animation; three
// key names share a track each (scaleX / posX2, scaleY / zoom, angle / posY2).
enum Channel : uint8_t { kPosX, kPosY, kScaleX, kScaleY, kAlpha, kAngle, kColour, kChannelCount };

// A key's ease: how the value leaves and reaches it (a missing attribute is
// kBoth, an unknown name kSmooth).
//   kNone:   straight through the key on both sides;
//   kIn:     flat arriving at the key;
//   kOut:    flat leaving the key;
//   kBoth:   flat on both sides;
//   kSmooth: Catmull-Rom through the neighbouring keys (flat at the ends).
enum Ease : uint8_t { kNone, kIn, kOut, kBoth, kSmooth };

// loop="stop" or none holds the last frame, "loop" wraps, "pingpong" runs
// back and forth.
enum Loop : uint8_t { kStop, kLoop, kPingPong };

struct Key {
  float t = 0, v = 0;
  Ease ease = kBoth;
};

// A frame's <trigger type value>: audio (a ui4audio play name), reveal
// (next), shake (camera) and set_cursor (set).
struct Trigger {
  float t = 0;
  std::string type, value;
};

struct Animation {
  std::string name;
  Loop loop = kStop;
  float speed = 1;
  float duration = 0;  // the last frame's time (seconds, after `speed`)
  std::array<std::vector<Key>, kChannelCount> channels;
  std::vector<Trigger> triggers;

  bool has(Channel c) const { return !channels[c].empty(); }
  // The channel's value at animation time `t` (already wrapped), or
  // `fallback` when the animation has no key on it.
  float sample(Channel c, float t, float fallback) const;
  // An instance's elapsed time folded by the loop mode.
  float wrap(float elapsed) const;
  // True when a play-once animation has reached its last frame.
  bool done(float elapsed) const { return loop == kStop && elapsed >= duration; }
  // The triggers whose times fall in (t0, t1] of elapsed time (play-once
  // animations only; the looping ones' triggers fire at each pass).
  std::vector<const Trigger*> crossed(float t0, float t1) const;
};

// The channel a key's type names; false for an unknown type.
bool channel_of(std::string_view type, Channel& out);
Ease ease_of(std::string_view name, bool present);
Loop loop_of(std::string_view name, Loop fallback);

// One animation from its <animation> element.
Animation parse_animation(const audio::XmlNode& node);

// A profile's <animations> by name. Where a name is defined twice (the
// fullscreen profile's two "pulse"), the first stays.
struct Library {
  std::map<std::string, Animation, std::less<>> by_name;
  const Animation* find(std::string_view name) const;
};
void parse_animations(const audio::XmlNode& animations, Library& out);

// A symbolizer's <animation event animation> (or <Animation ...>) bindings.
using Bindings = std::map<std::string, std::string, std::less<>>;
Bindings parse_bindings(const audio::XmlNode& symbolizer);

// The values an icon draws with: its own scale, alpha and turn from the
// animations its events play.
struct IconLook {
  float scale_x = 1, scale_y = 1, alpha = 1, angle_deg = 0;
};
// Applies one animation at elapsed time `elapsed` over `look` (channels the
// animation lacks keep their value).
void apply(const Animation& a, float elapsed, IconLook& look);

// The map's camera during a camera animation (the *_unlock ones). The title
// samples posX, posY (the world X and Z the camera goes to), scaleY (the
// zoom, a fraction of the UI/Map Map_Distance_Min..Max range of camera
// distances) and alpha (the weight from the player's camera to the
// animation's), each frame, and fires the triggers it passes.
struct CameraMove {
  double x = 0, z = 0;    // the target (world metres)
  double zoom = 0;        // the target's fraction of the distance range
  double weight = 0;      // 0: the player's camera, 1: the target
  bool has_target = false, has_zoom = false;
};
CameraMove camera_at(const Animation& a, float elapsed);

// The UI/Map tunables' defaults: the camera distance at zoom fraction 0 and 1.
inline constexpr double kMapDistanceMin = 200.0;
inline constexpr double kMapDistanceMax = 7652.0;
inline double map_distance(double fraction) { return kMapDistanceMin + (kMapDistanceMax - kMapDistanceMin) * fraction; }
// UI/Map/Zoom_Default (0.35) and UI/Map/Cursor_Zoom_Min / _Max (0.1, 0.4):
// the fraction the map opens at and, as read here, the player's zoom range
// (2.97 times in, 1.13 out from the opening distance; the captures measured
// about 2.6 in and 1/6 out). Q241.
inline constexpr double kZoomDefault = 0.35, kCursorZoomMin = 0.1, kCursorZoomMax = 0.4;

// The title advances a map animation by the frame's time, at most 1/30 s.
inline constexpr float kMaxStep = 1.0f / 30.0f;

}  // namespace carview::mapanim
