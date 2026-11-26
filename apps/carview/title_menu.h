// The front end's model (120_P_PRE_SPLASH, docs/UI_SCENE_SCREENS.md): Press
// Start, the profile step, then the original's two-row menu, SINGLE PLAYER and
// MULTIPLAYER. SINGLE PLAYER makes the career choice itself, as the original
// does: a played save continues where it stood, no save starts the
// first-time career. The engine's own entries (NEW CAREER, FREE ROAM,
// SETTINGS, QUIT) sit behind one EXTRAS list (X), as the pause menu's do, so
// the original rows stay as they were. No renderer dependency, so ui_tests
// can check it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace carview {

struct TitleMenu {
  enum class Phase : uint8_t {
    kPressStart,  // the movie, the logo and PRESS START
    kProfile,     // after Press Start: the profile is read, then the menu shows
    kVideo,       // a new profile's VIDEO screen (830_GAMMA_ADJUST), before the menu
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
    kSaveVideo,       // VIDEO's A SAVE: keep `brightness` and `subtitles`
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
  // VIDEO (Q276, Q363): a new profile only (a profile's boot goes from Press
  // Start to the menu, first30j boot-profile2, first30l vid2). BRIGHTNESS opens
  // at 60 (Main/Gamma = 0.6, step 0.05 = 5 points) and SUBTITLES at DISABLE
  // when UI and audio languages match; Y resets both. Held left/right repeats
  // on BRIGHTNESS after ~0.40 s at ~0.10 s/step (first30l vid1 h0542..h0548).
  static constexpr int kDefaultBrightness = 60;
  static constexpr int kBrightnessStep = 5;
  static constexpr double kSliderRepeatDelay = 0.40;
  static constexpr double kSliderRepeatInterval = 0.10;
  static constexpr int snap_brightness(int b) {
    const int c = b < 0 ? 0 : (b > 100 ? 100 : b);
    return ((c + 2) / 5) * 5;
  }
  enum class VideoRow : uint8_t { kBrightness, kSubtitles };

  Phase phase = Phase::kPressStart;
  Popup popup = Popup::kNone;
  int selected = 0;        // in rows()
  int menu_selected = 0;   // the menu row kept while EXTRAS is open
  double phase_t = 0.0;    // seconds in `phase`
  bool has_save = false;       // a played career (SINGLE PLAYER continues it)
  bool profile_error = false;  // the save exists but did not read
  bool first_profile = false;  // no save yet: Press Start leads to VIDEO
  int video_row = 0;           // VideoRow
  int brightness = kDefaultBrightness;  // 0..100
  bool subtitles = false;
  bool default_subtitles = false;
  int video_hold_dir = 0;
  double video_hold_t = 0.0;

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
      if (first_profile && !profile_error) {
        phase = Phase::kVideo;
        phase_t = 0.0;
        video_row = 0;
        return;
      }
      show_menu();
      if (profile_error) popup = Popup::kProfileCorrupt;
    }
  }
  // VIDEO's left and right: BRIGHTNESS by a step (5), SUBTITLES between ENABLE
  // and DISABLE.
  Step adjust(int dir) {
    if (phase != Phase::kVideo || dir == 0) return {};
    if (video_row == int(VideoRow::kBrightness)) {
      const int b = std::clamp(brightness + (dir > 0 ? kBrightnessStep : -kBrightnessStep), 0, 100);
      if (b == brightness) return {};
      brightness = b;
    } else {
      subtitles = !subtitles;
    }
    return {Action::kNone, Cue::kMove};
  }
  // Held left/right repeat on BRIGHTNESS (dir = -1, 0, +1).
  void set_video_hold(int dir) {
    const int d = dir < 0 ? -1 : (dir > 0 ? 1 : 0);
    if (d != video_hold_dir) {
      video_hold_dir = d;
      video_hold_t = 0.0;
    }
  }
  Step step_video_hold(double dt) {
    if (phase != Phase::kVideo || video_row != int(VideoRow::kBrightness) || video_hold_dir == 0 || dt <= 0.0) {
      if (phase != Phase::kVideo) video_hold_dir = 0, video_hold_t = 0.0;
      return {};
    }
    video_hold_t += dt;
    Step last{};
    while (video_hold_t >= kSliderRepeatDelay) {
      const Step s = adjust(video_hold_dir);
      video_hold_t -= kSliderRepeatInterval;
      if (s.cue != Cue::kNone) last = s;
      else break;
    }
    return last;
  }
  // VIDEO's Y RESET TO DEFAULT.
  Step reset_video() {
    if (phase != Phase::kVideo) return {};
    video_hold_dir = 0;
    video_hold_t = 0.0;
    brightness = kDefaultBrightness;
    subtitles = default_subtitles;
    return {Action::kNone, Cue::kAccept};
  }
  Step move(int by) {
    if (phase == Phase::kVideo) {
      video_hold_dir = 0;
      video_hold_t = 0.0;
      video_row = ((video_row + by) % 2 + 2) % 2;
      return {Action::kNone, Cue::kMove};
    }
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
      case Phase::kVideo:  // A SAVE: on to the menu, VIDEO not again
        first_profile = false;
        video_hold_dir = 0;
        video_hold_t = 0.0;
        show_menu();
        return {Action::kSaveVideo, Cue::kAccept};
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
      case Phase::kProfile:
      case Phase::kVideo: return {};  // VIDEO's help bar has no B
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
