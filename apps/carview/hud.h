// carview --drive: the on-screen HUD (speed, gear, rpm, ABS), drawn on the
// CPU into an opaque panel the renderer copies over each frame
// (Renderer::set_overlay). Ours: the game's own HUD is not specified.
#pragma once

#include <algorithm>
#include <cmath>
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
  // The pause menu (ours): a title and its items at twice the size, above
  // the banner, the selected one marked; empty: closed.
  std::string menu_title;
  std::vector<std::string> menu;
  int menu_selected = 0;
};

// The windowed drive's pause menu: Esc or Start opens it; the items move
// with up and down and wrap. Adapts cleanly between Free Roam and In-Race.
struct PauseMenu {
  enum Action {
    kResume,
    kMap,
    kEvents,
    kGarage,
    kUpgrades,
    kPaint,
    kAutoshow,
    kCareer,
    kRally,
    kClub,
    kCarClub,
    kAchievements,
    kPhotoMode,
    kReplay,
    kSettings,
    kReset,
    kRestartRace,
    kDifficulty,
    kQuitRace,
    kQuitToTitle,
    kQuit
  };
  bool open = false;
  bool in_race = false;
  bool in_rally_world = false;
  std::string events_label;      // e.g. "COLLECT GREEN WRISTBAND!" or "RACE CENTRAL"
  std::string difficulty_label;  // e.g. "< DIFFICULTY: MED (+15%) >"
  int selected = 0;
  std::vector<Action> items() const {
    if (in_race) {
      return {kResume, kRestartRace, kPhotoMode, kReplay, kMap, kDifficulty, kSettings, kQuitRace};
    }
    return {kResume, kMap, kEvents, kRally, kPhotoMode, kReplay, kGarage, kUpgrades, kPaint, kAutoshow, kCareer, kClub,
            kCarClub, kAchievements, kSettings, kQuitToTitle};
  }
  Action selected_action() const {
    const auto list = items();
    if (list.empty()) return kResume;
    return list[size_t(std::clamp(selected, 0, int(list.size()) - 1))];
  }
  std::string label(Action a) const;
  void show(bool racing = false) {
    open = true;
    in_race = racing;
    selected = 0;
  }
  void move(int by) {
    const int n = int(items().size());
    if (n > 0) selected = ((selected + by) % n + n) % n;
  }
  // Fills the HUD's menu fields (nothing while closed).
  void fill(HudState& st) const;
};

// Deterministic Left-Stick -> cardinal menu direction translator with
// engage/release hysteresis, axis-dominance lock (prevents diagonal bleed),
// and initial/steady auto-repeat.
struct MenuStickNav {
  enum class Dir : int { kNone = 0, kUp, kDown, kLeft, kRight };

  double engage_threshold = 0.55;
  double release_threshold = 0.35;
  double dominance_ratio = 1.25;
  double initial_delay_s = 0.38;
  double repeat_interval_s = 0.11;

  Dir held = Dir::kNone;
  double next_fire_s = 0.0;
  bool latched_neutral = false;

  void reset() {
    held = Dir::kNone;
    next_fire_s = 0.0;
    latched_neutral = false;
  }

  void latch_until_neutral() {
    held = Dir::kNone;
    next_fire_s = 0.0;
    latched_neutral = true;
  }

  // `lx`, `ly` in [-1, 1] (SDL convention: +x right, +y down).
  Dir update(double lx, double ly, double now_s) {
    const double ax = std::fabs(lx);
    const double ay = std::fabs(ly);

    if (latched_neutral) {
      if (ax < release_threshold && ay < release_threshold) latched_neutral = false;
      return Dir::kNone;
    }

    if (held != Dir::kNone) {
      bool still_held = false;
      switch (held) {
        case Dir::kUp: still_held = (ly <= -release_threshold); break;
        case Dir::kDown: still_held = (ly >= release_threshold); break;
        case Dir::kLeft: still_held = (lx <= -release_threshold); break;
        case Dir::kRight: still_held = (lx >= release_threshold); break;
        case Dir::kNone: break;
      }
      if (!still_held) {
        held = Dir::kNone;
      } else {
        if (now_s >= next_fire_s) {
          next_fire_s = now_s + repeat_interval_s;
          return held;
        }
        return Dir::kNone;
      }
    }

    Dir cand = Dir::kNone;
    if (ay >= engage_threshold && ay >= ax * dominance_ratio) {
      cand = (ly > 0.0) ? Dir::kDown : Dir::kUp;
    } else if (ax >= engage_threshold && ax >= ay * dominance_ratio) {
      cand = (lx > 0.0) ? Dir::kRight : Dir::kLeft;
    }

    if (cand != Dir::kNone) {
      held = cand;
      next_fire_s = now_s + initial_delay_s;
      return held;
    }
    return Dir::kNone;
  }
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

// The radio's popup (SPEC-RADIO "HUD"): the station's logo, the track's
// title and its artist on a pink label, right-aligned; RADIO OFF beside the
// off logo. `alpha` fades the content over the panel (the overlay is
// opaque). The logo is about 44 pixels tall at 720.
struct RadioPopupState {
  const uint8_t* logo = nullptr;  // straight-alpha RGBA, or null
  uint32_t logo_width = 0, logo_height = 0;
  std::string title, artist;
  bool off = false;
  double alpha = 1;
};
HudImage draw_radio_popup(const RadioPopupState& state, int height);

}  // namespace carview
