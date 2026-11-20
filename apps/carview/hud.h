// carview --drive: the on-screen HUD (speed, gear, rpm, ABS), drawn on the
// CPU into an opaque panel the renderer copies over each frame
// (Renderer::set_overlay). Ours: the game's own HUD is not specified.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carview {

struct HudState {
  double kmh = 0;        // shown as a magnitude
  int gear = 1;          // < 0 reverse, 0 neutral
  double rpm = 0;
  double redline_rpm = 7000;
  bool abs = false;
  // A line of text over the panel (the car, the picker's choice), shown in
  // upper case; empty: none.
  std::string banner;
};

struct HudImage {
  std::vector<uint8_t> rgba;  // sRGB-encoded RGBA8, opaque
  uint32_t width = 0, height = 0;
};

// `scale` is the size of one font pixel in screen pixels (1 or more).
HudImage draw_hud(const HudState& state, int scale);

// The scale for a window `height` pixels tall: the panel takes about a
// ninth of it.
int hud_scale(int height);

}  // namespace carview
