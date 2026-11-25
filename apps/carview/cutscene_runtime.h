// carview's in-engine cutscene playback: the disc's free-roam cutscenes
// (libs/cutscene) driving the render camera, the full-screen black and white
// fades the flows script (CBlackOut, CWhiteFlash, CFadeIn, CFadeOut) and the
// cutscenes' own FadeOptions, and the events the host acts on (place-car,
// audio, voice-over, mixer snapshots). In-engine cutscenes are not skipped
// (interim, SPEC_QUESTIONS Q61 g); movies skip on their own.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cutscene/cutscene.h"
#include "drive/cameras.h"
#include "race/first_time_career.h"

namespace drive {
class Vehicle;
}

namespace carview {

class CutsceneRuntime {
 public:
  // Loads the library from the disc once; later calls return the first result.
  bool ensure_loaded(const std::filesystem::path& game_root);
  const std::string& error() const { return error_; }
  const cutscene::Library& library() const { return lib_; }
  // The race cutscenes instead (a race's own runtime): the default set and
  // the track's (Tracks.DevName); reloads when the track changes. Stops
  // what is playing first (the player points into the library).
  bool load_race(const std::filesystem::path& game_root, const std::string& dev_name);
  // Plays a race_specific cutscene: the track's <name>_<DevName>, else the default.
  bool play_race(std::string_view name);
  // The route's named frames the cams can target (start_location_00, ...).
  void set_scene_nodes(std::vector<std::pair<std::string, cutscene::Frame>> nodes) {
    player_.set_scene_nodes(std::move(nodes));
  }

  // Starts a cutscene by name (false when it is not on the disc). `hold`
  // honours the cutscene's exit-on-finish voice-over.
  bool play(std::string_view name, bool hold);
  void stop();
  bool active() const { return player_.active(); }
  const cutscene::Cutscene* current() const { return player_.active() ? player_.cutscene() : nullptr; }
  double time() const { return player_.time(); }

  // Steps playback (not while frozen) and returns the events crossed.
  std::vector<const cutscene::Event*> update(double dt, bool vo_playing);
  // Capture: jump to `t` (events up to t fire and are returned) and freeze.
  std::vector<const cutscene::Event*> seek_and_freeze(double t);
  bool frozen() const { return frozen_; }

  // Scripted fades (FtcFade); they also advance in update().
  void fade(const race::FtcFade& f);
  void step_fades(double dt);
  // Drops the scripted fade (and the black a cut or fade-out left).
  void clear_fade() { fade_on_ = false; }
  // The overlay to draw over the frame: the scripted fade and the
  // cutscene's own fades, whichever covers more.
  cutscene::Overlay overlay() const;

  // The cutscene camera for the player car at `car_pos` / `car_heading_rad`
  // (engine heading, atan2(f.x, f.z)); false when no cutscene is playing.
  bool camera(const std::array<double, 3>& car_pos, double car_heading_rad, drive::CameraView& out) const;
  // The player car's parts the PartSpace cams follow (body axes, from the
  // car shown); empty = the car's origin.
  void set_car_parts(cutscene::CarParts parts) { parts_ = std::move(parts); }
  // The eye the world should stream around (the camera's, else nothing).
  bool streaming_focus(const std::array<double, 3>& car_pos, double car_heading_rad, std::array<double, 3>& out) const;

 private:
  cutscene::Library lib_;
  bool tried_ = false, loaded_ = false;
  std::string error_;
  std::string race_dev_;  // the track whose race set is loaded
  cutscene::Player player_;
  bool frozen_ = false;
  cutscene::CarParts parts_;
  // The scripted fade in progress (or the black it leaves behind).
  race::FtcFade fade_{};
  bool fade_on_ = false;
  double fade_t_ = 0.0;
};

// The player car's parts for the PartSpace cams (drive_first_time_career.cpp).
cutscene::CarParts car_parts(const drive::Vehicle& v, const std::array<float, 3>& bmin,
                             const std::array<float, 3>& bmax);

// CWhiteFlash's length: about 6 frames (UI_LAYOUTS; interim, Q61 e).
inline constexpr double kWhiteFlashSeconds = 0.1;

}  // namespace carview
