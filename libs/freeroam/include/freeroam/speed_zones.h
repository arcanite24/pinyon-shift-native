// Colorado Speed Traps (speed_camera.xml) & Speed Zones (average_speed.xml)
// matched to their Ribbon_00/GameObjs.xml gate posts (`_left`/`_right` and
// `_left1`/`_right1`/`_left2`/`_right2`). Detects gate crossings, computes
// instantaneous / average speed, tracks personal bests, and drives the camera
// flash and HUD banner state.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fh1 {
struct StringTable;
}

namespace freeroam {

struct SpeedGate {
  std::array<double, 3> left{};
  std::array<double, 3> right{};
  std::array<double, 3> center{};
  double width = 0.0;
};

struct SpeedTrapDef {
  std::string id;            // e.g. "speed_camera_01"
  std::string display_name;  // localized retail name from List_SpeedCameras.str (or fallback)
  int scoreboard_id = 0;     // 201+
  double min_speed_mph = 30.0;
  SpeedGate gate;
};

struct SpeedZoneDef {
  std::string id;            // e.g. "average_speed_01"
  std::string display_name;  // localized retail name from List_SpeedCameras.str (or fallback)
  int scoreboard_id = 0;     // 101+
  double min_speed_mph = 30.0;
  SpeedGate gate1;
  SpeedGate gate2;
  std::array<double, 3> midpoint{};
  double corridor_distance_m = 0.0;  // authored corridor length (or gate-to-gate span)
};

struct SpeedActivitiesData {
  std::vector<SpeedTrapDef> traps;
  std::vector<SpeedZoneDef> zones;

  const SpeedTrapDef* find_trap(std::string_view id) const;
  const SpeedZoneDef* find_zone(std::string_view id) const;
};

bool parse_speed_activities(std::string_view speed_camera_xml, std::string_view average_speed_xml,
                            std::string_view gameobjs_xml, SpeedActivitiesData& out, std::string* error = nullptr,
                            const fh1::StringTable* camera_strings = nullptr);
// `gamemodes_dir`: a world's own gamemodes folder (the Rally package's
// Media/gamemodes/ColoradoDirt) whose speed_camera.xml and average_speed.xml
// replace the disc's Colorado ones; empty: the disc's. The trap and zone
// gates are Colorado's GameObjs either way (ColoradoDirt ships none and
// names the same gates).
bool load_speed_activities(const std::filesystem::path& game_root, SpeedActivitiesData& out,
                           std::string* error = nullptr, const std::filesystem::path& gamemodes_dir = {});
// The Rally package's ColoradoDirt gamemodes folder under `package_dir`
// (empty when it has no speed_camera.xml).
std::filesystem::path colorado_dirt_gamemodes(const std::filesystem::path& package_dir);

struct SpeedActivityEvent {
  enum Kind { kTrapTriggered, kZoneEntered, kZoneCompleted, kZoneCancelled };
  Kind kind = kTrapTriggered;
  std::string id;
  std::string label;
  double speed_mph = 0.0;
  double prev_best_mph = 0.0;
  bool is_new_pb = false;
  bool flash = false;
  bool failed_off_road = false;
};

struct SpeedActivityHud {
  // Active Speed Zone in progress
  bool zone_active = false;
  std::string active_zone_id;
  double live_avg_mph = 0.0;
  double zone_distance_m = 0.0;
  double zone_elapsed_s = 0.0;
  // Recent trigger banner (Trap or Zone completion)
  bool banner_active = false;
  bool banner_is_zone = false;
  std::string banner_title;
  double banner_speed_mph = 0.0;
  double banner_best_mph = 0.0;
  bool banner_new_pb = false;
  double banner_at = -1e9;
  double banner_age_s = 0.0;  // seconds since the banner
  // A Speed Zone run just cancelled (off road, too slow, left the corridor):
  // its name and the live average it had, `abort_age_s` seconds ago.
  bool abort_active = false;
  std::string abort_title;
  double abort_speed_mph = 0.0;
  double abort_age_s = 0.0;
  // White camera flash intensity [0..1]
  float flash_alpha = 0.0f;
  // Off-road grace warning while active in a Speed Zone
  bool off_road_warning = false;
  double off_road_timer_s = 0.0;
};

class SpeedActivityTracker {
 public:
  static constexpr double kOffRoadFailGraceS = 1.5;

  explicit SpeedActivityTracker(const SpeedActivitiesData& data);

  void load_records(const std::vector<std::pair<std::string, double>>& traps,
                    const std::vector<std::pair<std::string, double>>& zones);
  std::vector<std::pair<std::string, double>> save_traps() const;
  std::vector<std::pair<std::string, double>> save_zones() const;

  double best_trap_mph(const std::string& id) const;
  double best_zone_mph(const std::string& id) const;
  size_t traps_completed() const { return best_traps_.size(); }
  size_t zones_completed() const { return best_zones_.size(); }

  std::vector<SpeedActivityEvent> step(double dt, double time, double x0, double z0, double x1, double z1,
                                       double speed_mps);
  // Cancels any active Speed Zone run cleanly (e.g. on Rewind).
  void cancel_active();
  // Records whether the car is currently off-road so step() fails the active Speed Zone
  // after continuous off-road time exceeds kOffRoadFailGraceS (~1.5 s).
  void set_off_road(bool off_road);
  bool zone_active() const { return active_zone_idx_ >= 0; }
  SpeedActivityHud hud(double time) const;
  void reset_session_clock();

 private:
  const SpeedActivitiesData& data_;
  std::map<std::string, double> best_traps_;
  std::map<std::string, double> best_zones_;
  std::map<std::string, double> trap_cd_;
  std::map<std::string, double> zone_cd_;

  // Active Speed Zone state
  int active_zone_idx_ = -1;
  int entry_gate_ = 0;  // 1 or 2
  double zone_elapsed_ = 0.0;
  double zone_dist_ = 0.0;
  double zone_corridor_progress_ = 0.0;
  double zone_slow_t_ = 0.0;
  bool off_road_ = false;
  double zone_off_road_t_ = 0.0;

  // Banner & flash state
  bool banner_is_zone_ = false;
  std::string banner_title_;
  double banner_speed_ = 0.0;
  double banner_best_ = 0.0;
  bool banner_new_pb_ = false;
  double banner_at_ = -1e9;
  double flash_at_ = -1e9;
  std::string abort_title_;
  double abort_speed_ = 0.0;
  double abort_at_ = -1e9;

  void note_abort(const SpeedZoneDef& z, double time);
};

}  // namespace freeroam
