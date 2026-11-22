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
    kClub,
    kPhotoMode,
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
  std::string events_label;      // e.g. "COLLECT GREEN WRISTBAND!" or "RACE CENTRAL"
  std::string difficulty_label;  // e.g. "< DIFFICULTY: MED (+15%) >"
  int selected = 0;
  std::vector<Action> items() const;
  Action selected_action() const;
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
