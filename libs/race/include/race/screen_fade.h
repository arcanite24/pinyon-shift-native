// The flows' screen fade: the CFadeIn behaviour of gamemodes.zip's
// state_definitions.xml and its FadeOptions (Duration, HoldDuration,
// CurveType, Color). The title's FadeOptions parser knows two curve names,
// Linear and S; every disc fade is black (Color 0, 0, 0, 0) and S. The
// campaign's stage transitions, the reset's fade and the load screen's
// hand-off all draw it (docs/UI_SCENE_ANIMATION.md, "Screen fades").
#pragma once

#include <algorithm>

namespace race {

struct ScreenFade {
  double duration = 1.0;
  double hold = 0.0;  // HoldDuration: the colour is held this long first
  bool s_curve = true;
};

// global.fade_in / global.fade_out (1 s) and global.fade_out_half_sec
// (0.5 s), all S curves and black.
inline constexpr ScreenFade kFlowFadeIn{1.0, 0.0, true};
inline constexpr ScreenFade kFlowFadeOut{1.0, 0.0, true};
inline constexpr ScreenFade kFlowFadeOutHalf{0.5, 0.0, true};

// The S curve, taken as smoothstep (3x^2 - 2x^3; Q61); Linear is x.
inline double screen_fade_curve(bool s_curve, double x) {
  x = std::clamp(x, 0.0, 1.0);
  return s_curve ? x * x * (3.0 - 2.0 * x) : x;
}

// How much of the screen the colour covers (0 clear, 1 covered) `t` s after
// the fade starts: a fade in goes from covered to clear, a fade out from
// clear to covered, each after its hold.
inline float screen_fade_cover(const ScreenFade& f, double t, bool fade_in) {
  if (t < f.hold) return fade_in ? 1.0f : 0.0f;
  const double x = f.duration > 0.0 ? (t - f.hold) / f.duration : 1.0;
  const double c = screen_fade_curve(f.s_curve, x);
  return float(std::clamp(fade_in ? 1.0 - c : c, 0.0, 1.0));
}

}  // namespace race
