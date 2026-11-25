// The pause map's cursor and zoom (docs/UI_SCENE_SCREENS.md, "The map"),
// measured on the original (map_pan_orig.png, map_zoom_in_orig.png and the
// frames around them):
// - The left stick moves a free cursor over a still map, about 490 px/s at
//   full deflection (1280x720 canvas), slower over an icon.
// - Past about 190 px left or right of the centre, or 120 px above or below,
//   pushing further scrolls the map instead, about 470 px/s; at the map's
//   edge the cursor goes on to x 96..1184, y 88..635.
// - The right stick zooms about the cursor: up to about 2.6 times the
//   opening scale, which is the furthest out, in under half a second.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace carview {

struct MapCursor {
  static constexpr float kSpeed = 490.0f;
  static constexpr float kSlowOverIcon = 0.45f;
  static constexpr float kScroll = 470.0f;
  static constexpr float kEdgeX = 190.0f, kEdgeY = 120.0f;
  static constexpr float kMinX = 96.0f, kMaxX = 1184.0f, kMinY = 88.0f, kMaxY = 635.0f;
  static constexpr float kCx = 640.0f, kCy = 360.0f;

  float x = kCx, y = kCy;

  // Moves the cursor by the stick (sx right, sy down, -1..1); returns the
  // canvas px the view should scroll (+x: the view moves right).
  std::array<float, 2> step(double dt, float sx, float sy, bool over_icon = false) {
    std::array<float, 2> scroll{0.0f, 0.0f};
    const float k = over_icon ? kSlowOverIcon : 1.0f;
    auto axis = [&](float& c, float s, float centre, float edge, float lo, float hi, float& out) {
      if (std::fabs(s) < 1e-4f) return;
      float n = c + s * kSpeed * k * float(dt);
      if (s > 0.0f && n > centre + edge) {
        out = s * kScroll * float(dt);
        n = std::max(c, centre + edge);
      } else if (s < 0.0f && n < centre - edge) {
        out = s * kScroll * float(dt);
        n = std::min(c, centre - edge);
      }
      c = std::clamp(n, lo, hi);
    };
    axis(x, sx, kCx, kEdgeX, kMinX, kMaxX, scroll[0]);
    axis(y, sy, kCy, kEdgeY, kMinY, kMaxY, scroll[1]);
    return scroll;
  }
  // The scroll the view could not take (at the map's edge) moves the cursor.
  void absorb(float dx, float dy) {
    x = std::clamp(x + dx, kMinX, kMaxX);
    y = std::clamp(y + dy, kMinY, kMaxY);
  }
  void reset() { x = kCx, y = kCy; }
};

// The zoom: `zoom` from `base` (the opening, furthest out) to base * kMaxIn.
struct MapZoom {
  static constexpr double kMaxIn = 2.6;
  static constexpr double kRate = 1.9;  // ln(2.6) in about half a second

  // The new zoom for the right stick's `axis` (up +) over dt.
  static double step(double zoom, double base, double axis, double dt) {
    if (std::fabs(axis) < 1e-4) return zoom;
    const double z = zoom * std::exp(axis * kRate * dt);
    return std::clamp(z, base, base * kMaxIn);
  }
};

}  // namespace carview
