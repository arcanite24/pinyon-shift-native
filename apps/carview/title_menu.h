// The front end's model (120_P_PRE_SPLASH, docs/UI_SCENE_SCREENS.md): Press
// Start, the profile step, then the original's two-row menu, SINGLE PLAYER and
// MULTIPLAYER. SINGLE PLAYER makes the career choice itself, as the original
// does: a played save continues where it stood, no save starts the
// first-time career. The engine's own entries (NEW CAREER, FREE ROAM,
// SETTINGS, QUIT) sit behind one EXTRAS list (X), as the pause menu's do, so
// the original rows stay as they were. No renderer dependency, so ui_tests
// can check it.
#pragma once

#include <cstdint>
#include <vector>

namespace carview {

struct TitleMenu {
  enum class Phase : uint8_t {
    kPressStart,  // the movie, the logo and PRESS START
    kProfile,     // after Press Start: the profile is read, then the menu shows
    kMenu,        // SINGLE PLAYER / MULTIPLAYER
    kExtras,      // the engine's own entries
  };
  enum class Row : uint8_t { kSinglePlayer, kMultiplayer, kNewCareer, kFreeRoam, kSettings, kQuit };
  // A message over the menu (H_MESSAGE_POPUP).
  enum class Popup : uint8_t {
    kNone,
    kNoNetwork,          // MULTIPLAYER offline: the original's NO NETWORK ACCESS
    kProfileCorrupt,     // the save could not be read: INVALID PROFILE
    kConfirmNewCareer,   // EXTRAS > NEW CAREER over a save (the engine's)
  };
  // What the app does after an input.
  enum class Action : uint8_t {
    kNone,
    kContinueCareer,  // SINGLE PLAYER with a played save: load into free roam
    kStartCareer,     // SINGLE PLAYER without one: the first-time career
    kNewCareer,       // EXTRAS > NEW CAREER (confirmed when a save exists)
    kFreeRoam,        // EXTRAS > FREE ROAM
    kSettings,        // EXTRAS > SETTINGS
    kQuit,            // EXTRAS > QUIT, or back at Press Start (PC)
  };
  // The sound an input plays (ui4audio.xml play names through UiEvent).
  enum class Cue : uint8_t { kNone, kMove, kAccept, kBack, kDeny };
  struct Step {
    Action action = Action::kNone;
    Cue cue = Cue::kNone;
  };

  // The profile step's length: the scene's Menu_Intro slide fades the logo
  // out over 0..0.299 s and the menu in over 0.493..1.013 s; the rows take
  // input from when the menu starts to show (inferred; no profile picker,
  // Q137).
  static constexpr double kProfileSeconds = 0.493;

  Phase phase = Phase::kPressStart;
  Popup popup = Popup::kNone;
  int selected = 0;        // in rows()
  int menu_selected = 0;   // the menu row kept while EXTRAS is open
  double phase_t = 0.0;    // seconds in `phase`
  bool has_save = false;       // a played career (SINGLE PLAYER continues it)
  bool profile_error = false;  // the save exists but did not read

  std::vector<Row> rows() const {
    if (phase == Phase::kMenu) return {Row::kSinglePlayer, Row::kMultiplayer};
    if (phase == Phase::kExtras) return {Row::kNewCareer, Row::kFreeRoam, Row::kSettings, Row::kQuit};
    return {};
  }
  Row selected_row() const {
    const auto r = rows();
    return r.empty() ? Row::kSinglePlayer : r[size_t(selected < 0 ? 0 : selected >= int(r.size()) ? 0 : selected)];
  }
  bool menu_visible() const { return phase == Phase::kMenu || phase == Phase::kExtras; }

  // Back to Press Start (boot, attract's end).
  void reset() {
    phase = Phase::kPressStart;
    popup = Popup::kNone;
    selected = menu_selected = 0;
    phase_t = 0.0;
  }
  // Straight to the menu, SINGLE PLAYER selected (quit to title, --title-menu).
  void show_menu() {
    phase = Phase::kMenu;
    popup = Popup::kNone;
    selected = menu_selected = 0;
    phase_t = 0.0;
  }
  void update(double dt) {
    phase_t += dt > 0.0 ? dt : 0.0;
    if (phase == Phase::kProfile && phase_t >= kProfileSeconds) {
      show_menu();
      if (profile_error) popup = Popup::kProfileCorrupt;
    }
  }
  Step move(int by) {
    if (popup != Popup::kNone) return {};
    const int n = int(rows().size());
    if (n <= 1) return {};
    selected = ((selected + by) % n + n) % n;
    return {Action::kNone, Cue::kMove};
  }
  // A row under the mouse.
  Step hover(int row) {
    if (popup != Popup::kNone || row < 0 || row >= int(rows().size()) || row == selected) return {};
    selected = row;
    return {Action::kNone, Cue::kMove};
  }
  Step open_extras() {
    if (phase != Phase::kMenu || popup != Popup::kNone) return {};
    menu_selected = selected;
    phase = Phase::kExtras;
    selected = 0;
    phase_t = 0.0;
    return {Action::kNone, Cue::kAccept};
  }
  Step confirm() {
    switch (popup) {
      case Popup::kNone: break;
      case Popup::kConfirmNewCareer:  // A YES (B NO is back())
        popup = Popup::kNone;
        return {Action::kNewCareer, Cue::kAccept};
      default:
        popup = Popup::kNone;
        return {Action::kNone, Cue::kAccept};
    }
    switch (phase) {
      case Phase::kPressStart:
        phase = Phase::kProfile;
        phase_t = 0.0;
        return {Action::kNone, Cue::kAccept};
      case Phase::kProfile: return {};
      case Phase::kMenu:
        if (selected_row() == Row::kMultiplayer) {
          popup = Popup::kNoNetwork;
          return {Action::kNone, Cue::kDeny};
        }
        return {has_save ? Action::kContinueCareer : Action::kStartCareer, Cue::kAccept};
      case Phase::kExtras:
        switch (selected_row()) {
          case Row::kNewCareer:
            if (has_save) {
              popup = Popup::kConfirmNewCareer;
              return {Action::kNone, Cue::kAccept};
            }
            return {Action::kNewCareer, Cue::kAccept};
          case Row::kFreeRoam: return {Action::kFreeRoam, Cue::kAccept};
          case Row::kSettings: return {Action::kSettings, Cue::kAccept};
          case Row::kQuit: return {Action::kQuit, Cue::kAccept};
          default: return {};
        }
    }
    return {};
  }
  Step back() {
    if (popup != Popup::kNone) {
      popup = Popup::kNone;
      return {Action::kNone, Cue::kBack};
    }
    switch (phase) {
      case Phase::kPressStart: return {Action::kQuit, Cue::kNone};
      case Phase::kProfile: return {};
      case Phase::kMenu:
        reset();
        return {Action::kNone, Cue::kBack};
      case Phase::kExtras:
        phase = Phase::kMenu;
        selected = menu_selected;
        phase_t = 0.0;
        return {Action::kNone, Cue::kBack};
    }
    return {};
  }
};

}  // namespace carview
