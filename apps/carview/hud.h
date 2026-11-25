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

// The pause menu (925_PAUSE_MENU, docs/UI_SCENE_SCREENS.md): Esc or Start
// opens it on the game's own rows, which move with up and down and wrap; B
// resumes. The engine's extra screens sit behind one EXTRAS list (X) so the
// original rows stay as they were; MESSAGE CENTER and SPONSOR CHALLENGES open
// inside the menu and B returns from them.
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
    kQuit,
    kMessageCentre,
    kSponsorChallenges
  };
  // What the open menu shows: its rows, the EXTRAS list, or a screen opened
  // from a row.
  enum class Screen { kList, kExtras, kMessageCentre, kSponsorChallenges };
  bool open = false;
  bool in_race = false;
  bool in_rally_world = false;
  Screen screen = Screen::kList;
  std::string events_label;      // e.g. "COLLECT GREEN WRISTBAND!" or "RACE CENTRAL"
  std::string difficulty_label;  // e.g. "< DIFFICULTY: MED (+5%) >"
  int selected = 0;              // in rows(): the menu's rows or the extras
  int list_selected = 0;         // the menu row kept while the extras are open
  int sponsor_selected = 0;      // Sponsor Challenges' selected sponsor
  // The selection the HUD last drew and when it changed (the cross-fade);
  // kept by the drawing side, hence mutable.
  mutable int shown_selected = -1, shown_from = -1;
  mutable double shown_at = -1e9;

  // The retail rows (925_PAUSE_MENU's Option_List): free roam MAP, SETTINGS
  // (the retail MULTIPLAYER slot), PHOTO MODE, MESSAGE CENTER, SPONSOR
  // CHALLENGES, MY PROFILE, QUIT; racing RESTART, MAP, PHOTO MODE, SPONSOR
  // CHALLENGES, MY PROFILE, QUIT RACE.
  std::vector<Action> items() const {
    if (in_race) return {kRestartRace, kMap, kPhotoMode, kSponsorChallenges, kCareer, kQuitRace};
    return {kMap, kSettings, kPhotoMode, kMessageCentre, kSponsorChallenges, kCareer, kQuitToTitle};
  }
  // The engine's own screens (EXTRAS).
  std::vector<Action> extras() const {
    if (in_race) return {kReplay, kDifficulty, kSettings};
    return {kEvents, kRally, kGarage, kUpgrades, kPaint, kAutoshow, kClub, kCarClub, kAchievements, kReplay};
  }
  std::vector<Action> rows() const { return screen == Screen::kExtras ? extras() : items(); }
  Action selected_action() const {
    const auto list = rows();
    if (list.empty()) return kResume;
    return list[size_t(std::clamp(selected, 0, int(list.size()) - 1))];
  }
  std::string label(Action a) const;
  void show(bool racing = false) {
    open = true;
    in_race = racing;
    screen = Screen::kList;
    selected = 0;
    list_selected = 0;
    shown_selected = -1;
  }
  void move(int by) {
    const int n = int(rows().size());
    if (n > 0) selected = ((selected + by) % n + n) % n;
  }
  void open_extras() {
    if (screen != Screen::kList) return;
    list_selected = selected;
    screen = Screen::kExtras;
    selected = 0;
    shown_selected = -1;
  }
  // Opens a screen from a row (MESSAGE CENTER, SPONSOR CHALLENGES).
  void open_screen(Screen s) {
    list_selected = selected;
    screen = s;
    sponsor_selected = 0;
  }
  // B: back to the rows from the extras or a screen (true), or resume
  // (false: the menu closes).
  bool back() {
    if (screen != Screen::kList) {
      if (screen == Screen::kExtras) shown_selected = -1;
      screen = Screen::kList;
      selected = list_selected;
      return true;
    }
    open = false;
    return false;
  }
  // Notes the selection drawn at time `t` (the row cross-fade's start).
  void note_shown(double t) const {
    if (selected == shown_selected) return;
    shown_from = shown_selected;
    shown_selected = selected;
    shown_at = shown_from < 0 ? -1e9 : t;
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
