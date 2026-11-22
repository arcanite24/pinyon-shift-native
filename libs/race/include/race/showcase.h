// Airborne Showcases (PLANE_RACE_001 .. PLANE_RACE_007, CareerTypeId = 8):
// animated 3D airborne opponent (P-51 Mustang, Hot-Air Balloon, Stunt Jet,
// Biplane, Attack Helicopter), flight path & calibrated timing curve over the
// showcase route, progress & distance-gap evaluation, and finish verdict.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "drive/ai_driver.h"

namespace race {

enum class ShowcaseAircraftKind {
  kMustangP51,     // PLANE_RACE_001 (P-51 Mustang), PLANE_RACE_005 (Aerobatic Plane)
  kHotAirBalloon,  // PLANE_RACE_002 (Hot-Air Balloon)
  kStuntJet,       // PLANE_RACE_003 (Stunt Jet)
  kBiplane,        // PLANE_RACE_004, PLANE_RACE_007 (Biplane)
  kHelicopter,     // PLANE_RACE_006 (Helicopter)
};

struct ShowcaseSpec {
  std::string horizon_id;
  ShowcaseAircraftKind kind = ShowcaseAircraftKind::kMustangP51;
  std::string display_name;       // e.g. "P-51 MUSTANG"
  double base_altitude_m = 26.0;  // cruise height above road
  double swoop_altitude_m = 11.0; // low-pass height near start/finish/midpoint
  double lateral_sway_m = 14.0;   // scenic S-curve sweep across the road
  double target_speed_mps = 48.0; // average route progress speed (calibrated to car class)
  double scale = 1.0;
};

// True for all 7 Airborne Showcase events (CareerTypeId == 8 or PLANE_RACE_*).
bool is_airborne_showcase(int career_type, std::string_view horizon_id);
// Returns the tuned spec for one of the 7 Airborne Showcases.
ShowcaseSpec showcase_spec_for(std::string_view horizon_id);

struct ShowcasePose {
  std::array<double, 3> position{};  // world (x, y, z)
  double heading = 0.0;              // rad, 0 faces +z, positive toward +x
  double pitch = 0.0;                // rad, positive nose up
  double roll = 0.0;                 // rad, banking into turns
  double rotor_angle = 0.0;          // spinning propeller / main rotor phase (rad)
  double progress_m = 0.0;           // distance along showcase racing line
  double fraction = 0.0;             // [0..1] fraction of route completed
  double speed_mps = 0.0;
  bool finished = false;
  double finish_time = -1.0;
};

class ShowcaseOpponent {
 public:
  void init(const ShowcaseSpec& spec, const drive::RacingLine& line, double start_progress, double finish_progress,
            double expected_car_speed_mps = 0.0);
  void step(double dt, double race_clock, double player_progress);

  const ShowcaseSpec& spec() const { return spec_; }
  const ShowcasePose& pose() const { return pose_; }
  bool active() const { return active_; }
  // Signed gap in metres: positive when player is ahead of the aircraft.
  double player_gap_m(double player_progress) const { return player_progress - pose_.progress_m; }
  // 1 if player is ahead (or beat the aircraft to the finish), 2 otherwise.
  int player_place(double player_progress, bool player_finished, double player_finish_time) const;

 private:
  bool active_ = false;
  ShowcaseSpec spec_;
  const drive::RacingLine* line_ = nullptr;
  double start_p_ = 0.0;
  double finish_p_ = 1000.0;
  double target_speed_ = 48.0;
  double prev_heading_ = 0.0;
  ShowcasePose pose_;
};

}  // namespace race
