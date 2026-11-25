// Airborne Showcases (PLANE_RACE_001 .. PLANE_RACE_007, CareerTypeId = 8):
// animated 3D airborne opponent (P-51 Mustang, Helicopter, Biplane, Hot-Air
// Balloon), XML-driven airborne_challenges.xml parser, spring rubber-banding &
// timing checkpoints over the showcase route, proximity rumble curve, and
// finish verdict.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drive/ai_driver.h"

namespace race {

enum class ShowcaseAircraftKind {
  kMustangP51,     // Event 101 (PLANE_RACE_001), Event 222 (PLANE_RACE_006)
  kHotAirBalloon,  // Event 223 (PLANE_RACE_004)
  kStuntJet,       // Unused legacy extension value (no retail showcase uses a jet)
  kBiplane,        // Event 172 (PLANE_RACE_003), Event 246 (PLANE_RACE_007)
  kHelicopter,     // Event 168 (PLANE_RACE_002), Event 220 (PLANE_RACE_005)
};

struct ShowcaseTimingCheckpoint {
  double time_s = 0.0;
  double spline_dist_to_next = 0.0;
  double additional_rb_offset = 0.0;
};

struct ShowcaseDifficultySpeeds {
  double base_mult = 1.0;
  double rubber_band_min_mult = 0.75;
  double rubber_band_max_mult = 1.3;
};

struct ShowcaseSpec {
  int64_t event_id = 0;
  std::string horizon_id;
  ShowcaseAircraftKind kind = ShowcaseAircraftKind::kMustangP51;
  std::string display_name;       // e.g. "P-51 MUSTANG"
  // <Opponent LocStringPilotName>: the AirChallenges string naming the
  // opponent ("IDS_Pilot2" -> "Plane"); empty for the built-in specs.
  std::string pilot_name_ids;
  std::string anim_pgeo;          // e.g. "ANIM_Showcase_Event_Helicopter_Race.pgeo"
  std::string anim_in_race;       // in-race animation name
  double base_altitude_m = 26.0;  // cruise height above road
  double swoop_altitude_m = 11.0; // low-pass height near start/finish/midpoint
  double lateral_sway_m = 14.0;   // scenic S-curve sweep across the road
  double target_speed_mps = 48.0; // fallback average route progress speed
  double scale = 1.0;

  // airborne_challenges.xml timing, spring rubber-banding, and proximity rumble.
  bool on_countdown = false;
  double cross_start_time_s = 0.0;
  double cross_finish_time_s = 120.0;
  double rubber_band_spring = 30000.0;
  double rubber_band_accel_rate = 0.4;
  double rubber_band_decel_rate = 0.15;
  double rubber_band_offset_ahead = 0.0;
  double rubber_band_checkpoint_adjustment = 0.0;
  double base_mult = 1.0;
  double rubber_band_min_mult = 0.75;
  double rubber_band_max_mult = 1.3;
  std::array<ShowcaseDifficultySpeeds, 4> difficulty_speeds{};  // Easy, Med, Hard, Pro
  double rumble_strength = 0.8;
  double rumble_distance_outer_m = 80.0;
  double rumble_distance_inner_m = 15.0;
  std::vector<ShowcaseTimingCheckpoint> timing_checkpoints;
};

// True for all 7 Airborne Showcase events (CareerTypeId == 8 or PLANE_RACE_*).
bool is_airborne_showcase(int career_type, std::string_view horizon_id);
// Returns the built-in spec for one of the 7 Airborne Showcases by horizon_id.
ShowcaseSpec showcase_spec_for(std::string_view horizon_id);
// Returns the built-in spec by numeric Events.Id (101, 168, 172, 223, 220, 222, 246),
// falling back to `horizon_id` when unknown.
ShowcaseSpec showcase_spec_for_event(int64_t event_id, std::string_view horizon_id = {});

// Parses `airborne_challenges.xml` into `out` (tolerant of retail XML quirks).
bool parse_airborne_challenges_xml(std::string_view xml, std::vector<ShowcaseSpec>& out, std::string* error = nullptr);
// Loads `<game_root>/media/tracks/colorado/airborne_challenges.xml`.
bool load_airborne_challenges(const std::filesystem::path& game_root, std::vector<ShowcaseSpec>& out,
                              std::string* error = nullptr);
// Resolves an event's spec from parsed XML specs (falling back to built-in defaults)
// and applies the selected difficulty tier (0=Easy, 1=Med, 2=Hard, 3=Pro).
ShowcaseSpec resolve_showcase_spec(int64_t event_id, std::string_view horizon_id,
                                   const std::vector<ShowcaseSpec>& xml_specs, int difficulty = 1);

// Optional terrain elevation probe returning the highest solid surface Y (m) at world (x, z).
using GroundHeightFn = std::function<std::optional<double>(double x, double z)>;

struct ShowcasePose {
  std::array<double, 3> position{};  // world (x, y, z)
  double heading = 0.0;              // rad, 0 faces +z, positive toward +x
  double pitch = 0.0;                // rad, positive nose up
  double roll = 0.0;                 // rad, positive banks left (raises +X right wing)
  double rotor_angle = 0.0;          // spinning propeller / main rotor phase (rad)
  double progress_m = 0.0;           // distance along showcase racing line
  double fraction = 0.0;             // [0..1] fraction of route completed
  double speed_mps = 0.0;
  double ground_y = 0.0;             // highest terrain elevation sampled beneath aircraft (m)
  double agl_m = 0.0;                // altitude above ground directly beneath aircraft (m)
  bool finished = false;
  double finish_time = -1.0;
};

// World-space aircraft basis and VFX emitter attachment points derived from ShowcaseSpec + ShowcasePose.
struct ShowcaseVfxAnchors {
  std::array<double, 3> forward{0.0, 0.0, 1.0};   // unit forward (+Z local)
  std::array<double, 3> right{1.0, 0.0, 0.0};     // unit right (+X local)
  std::array<double, 3> up{0.0, 1.0, 0.0};        // unit up (+Y local)
  std::array<double, 3> velocity{0.0, 0.0, 0.0};  // world velocity (m/s)
  std::array<std::array<double, 3>, 2> exhaust_pos{};
  int exhaust_count = 0;
  std::array<std::array<double, 3>, 2> wingtip_pos{};
  int wingtip_count = 0;
  std::array<double, 3> prop_hub_pos{};
  std::array<double, 3> ground_wash_pos{};
  double agl_m = 0.0;
};

// Transforms local aircraft directions/points into game world space matching the 3D showcase mesh.
std::array<double, 3> showcase_local_dir(const ShowcasePose& pose, double lx, double ly, double lz);
std::array<double, 3> showcase_local_point(const ShowcaseSpec& spec, const ShowcasePose& pose,
                                           double lx, double ly, double lz);
ShowcaseVfxAnchors compute_showcase_vfx_anchors(const ShowcaseSpec& spec, const ShowcasePose& pose);

class ShowcaseOpponent {
 public:
  void init(const ShowcaseSpec& spec, const drive::RacingLine& line, double start_progress, double finish_progress,
            double expected_car_speed_mps = 0.0);
  void apply_initial_clearance(const GroundHeightFn& ground_height);
  void step(double dt, double race_clock, double player_progress, const GroundHeightFn& ground_height = {});

  const ShowcaseSpec& spec() const { return spec_; }
  const ShowcasePose& pose() const { return pose_; }
  bool active() const { return active_; }
  double min_clearance_m() const;
  double anim_time() const { return anim_time_; }
  double anim_speed_mult() const { return anim_speed_mult_; }
  // Signed gap in metres: positive when player is ahead of the aircraft.
  double player_gap_m(double player_progress) const { return player_progress - pose_.progress_m; }
  // 1 if player is ahead (or beat the aircraft to the finish), 2 otherwise.
  int player_place(double player_progress, bool player_finished, double player_finish_time) const;
  // Proximity rumble / camera shake strength [0..spec.rumble_strength] at distance `dist_m`.
  double near_rumble_strength(double dist_m) const;

  struct Snapshot {
    ShowcasePose pose;
    double prev_heading = 0.0;
    double anim_time = 0.0;
    double anim_speed_mult = 1.0;
    double altitude_y = 0.0;
    double smoothed_pitch = 0.0;
    double smoothed_roll = 0.0;
    bool altitude_initialized = false;
  };
  Snapshot snapshot() const {
    return {pose_, prev_heading_, anim_time_, anim_speed_mult_, altitude_y_, smoothed_pitch_, smoothed_roll_,
            altitude_initialized_};
  }
  void restore(const Snapshot& s) {
    pose_ = s.pose;
    prev_heading_ = s.prev_heading;
    anim_time_ = s.anim_time;
    anim_speed_mult_ = s.anim_speed_mult;
    altitude_y_ = s.altitude_y;
    smoothed_pitch_ = s.smoothed_pitch;
    smoothed_roll_ = s.smoothed_roll;
    altitude_initialized_ = s.altitude_initialized;
  }

 private:
  void build_checkpoint_table();
  double frac_from_anim_time(double t, double* extra_rb_offset = nullptr) const;

  bool active_ = false;
  ShowcaseSpec spec_;
  const drive::RacingLine* line_ = nullptr;
  double start_p_ = 0.0;
  double finish_p_ = 1000.0;
  double target_speed_ = 48.0;
  double nominal_duration_s_ = 120.0;
  double anim_time_ = 0.0;
  double anim_speed_mult_ = 1.0;
  double prev_heading_ = 0.0;
  double altitude_y_ = 0.0;
  double smoothed_pitch_ = 0.0;
  double smoothed_roll_ = 0.0;
  bool altitude_initialized_ = false;
  bool use_xml_checkpoints_ = false;
  std::vector<double> cp_times_;
  std::vector<double> cp_fracs_;
  std::vector<double> cp_rb_offsets_;
  ShowcasePose pose_;
};

}  // namespace race
