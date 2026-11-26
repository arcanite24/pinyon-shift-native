// Scripted career / cutscene cars driven by CCutsceneCarTrigger and
// CCutsceneReleaseCarTrigger (e.g. Darius Flynt's Ferrari 599XX leading the
// opening Viper drive, the 5 Diner parking-lot cars, and the Race to Festival
// rivals). Pure headless state & road-following logic; rendering is handled
// by carview's guest Showroom pipeline.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "traffic/roads.h"

namespace traffic {

struct ScriptedCar {
  uint32_t id = 0;
  std::string name;
  int64_t car_id = 0;
  std::string car_media;
  int64_t paint_rgb = -1;  // 0xRRGGBB override, or -1 for the model's default swatch
  P3 position{};
  double heading = 0.0;  // radians; forward = (sin h, 0, cos h)
  double pitch = 0.0;    // radians; positive nose up
  double speed = 0.0;    // m/s
  double target_speed = 0.0;
  double steer_angle = 0.0;  // front wheel visual steer (rad)
  bool released = false;
  bool braking = false;
  bool snap_to_ground = false;
  bool ground_snapped = false;
  bool pace_ahead = false;  // gently keep visible ahead of player (Flynt initial drive)
  double age = 0.0;
  double despawn_after = 0.0;  // <= 0 means stay until explicitly cleared
  // Road-following state once released onto a traffic::Roads graph.
  int road = -1;
  int dir = 1;
  double s = 0.0;
  double lateral = 0.0;
  double target_lateral = 2.0;
};

class ScriptedCars {
 public:
  using GroundFn = std::function<bool(double x, double y_hint, double z, double& out_y)>;

  struct SpawnParams {
    std::string name;
    int64_t car_id = 0;
    std::string car_media;
    int64_t paint_rgb = -1;
    P3 pos{};
    double heading_rad = 0.0;
    bool snap_to_ground = false;
    bool pace_ahead = false;
  };

  struct ReleaseParams {
    std::string name;  // accepts "FestivalCar_0" or "FestivalCar_0_Release"
    bool use_release_pose = false;
    P3 pos{};
    double heading_rad = 0.0;
    bool snap_to_ground = false;
    double initial_speed_mps = 0.0;
    double despawn_time_s = 0.0;
  };

  struct StepOptions {
    bool has_player = false;
    P3 player_pos{};
    double player_heading = 0.0;
    double player_speed = 0.0;
  };

  // Normalises a trigger name by stripping a trailing "_Release" suffix
  // (case-insensitive) so CCutsceneReleaseCarTrigger ("FestivalCar_0_Release")
  // matches its CCutsceneCarTrigger ("FestivalCar_0").
  static std::string canonical_name(std::string_view name);

  // Spawns or replaces a scripted car at its authored world pose (parked until
  // release() is called). Returns its unique runtime id.
  uint32_t spawn(const SpawnParams& p, const GroundFn& ground = nullptr);

  // Releases a previously spawned car to drive forward along the road network
  // (and optional guidance route) at `initial_speed_mps`. Returns false if no
  // matching car exists.
  bool release(const ReleaseParams& p, const Roads* roads = nullptr, const GroundFn& ground = nullptr);

  // Optional guidance route waypoints (e.g. sat-nav route to the Diner or
  // Festival) used to steer junction choices for released scripted cars.
  void set_guidance_route(std::vector<P3> waypoints);
  void clear_guidance_route() { guidance_.clear(); }

  // Advances released scripted cars by dt seconds and expires any car past its
  // `despawn_after` lifetime. Also lazily snaps parked cars to ground once
  // terrain collision becomes available.
  void step(double dt, const Roads* roads = nullptr, const GroundFn& ground = nullptr,
            const StepOptions& opts = {});

  const ScriptedCar* find(std::string_view name) const;
  ScriptedCar* find_mut(std::string_view name);
  bool erase(std::string_view name);
  void clear();

  const std::vector<ScriptedCar>& cars() const { return cars_; }
  bool empty() const { return cars_.empty(); }

 private:
  void attach_to_road(ScriptedCar& car, const Roads& roads);
  bool advance_junction(ScriptedCar& car, const Roads& roads, double overshoot);

  uint32_t next_id_ = 1;
  std::vector<ScriptedCar> cars_;
  std::vector<P3> guidance_;
};

}  // namespace traffic
