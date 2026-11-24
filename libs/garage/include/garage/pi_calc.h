// PI.xml virtual-lap Performance Index, class & stat-rating calculator
// (SPEC-GARAGE-UPGRADES, The index of a car with parts).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cars/physics.h"
#include "drive/vehicle.h"

namespace garage {

struct ClassBand;

struct PiSegment {
  double straight_m = 200.0;
  double corner_radius_m = 50.0;
  double corner_angle_deg = 90.0;
  // Precomputed Euler-spiral corner geometry.
  double effective_straight_m = 200.0;
  double entry_arc_m = 0.0;
  double exit_arc_m = 0.0;
  double unwind_power = 1.0;
  double overlap_m = 0.0;
};

struct PiConfig {
  double min_pi_time_s = 138.24;
  double max_pi_time_s = 279.90;
  double track_width_m = 9.0;
  // Drive-type factors indexed by 0=FWD, 1=RWD, 2=AWD.
  std::array<double, 3> corner_accel_grip{0.91, 0.85, 1.0};
  std::array<double, 3> straight_accel_grip{0.955, 0.925, 1.0};
  std::array<double, 3> lap_time_scale{1.0, 1.0, 1.0};
  std::vector<double> gear_redline_mph;
  std::vector<PiSegment> segments;
  bool loaded_from_xml = false;

  void finalize_segments();
};

// Parses PI.xml text and precomputes Euler-spiral corner segments.
bool parse_pi_xml(std::string_view xml, PiConfig& out, std::string* error = nullptr);
// Loads media/physics.zip member PI.xml.
bool load_pi_config(const std::filesystem::path& physics_zip, PiConfig& out, std::string* error = nullptr);
// Minimal synthetic fallback config for tests without game assets.
PiConfig synthetic_pi_config();

struct CarStats {
  double speed_rating = 0.0;         // 0..10
  double handling_rating = 0.0;      // 0..10
  double accel_rating = 0.0;         // 0..10
  double launch_rating = 0.0;        // 0..10
  double braking_rating = 0.0;       // 0..10
  double peak_power_kw = 0.0;        // kW (1 kW = 1.34102 hp)
  double peak_torque_nm = 0.0;       // N*m
  double curb_weight_kg = 0.0;       // kg
  double weight_dist_front = 0.5;    // [0..1]
  double top_speed_ms = 0.0;         // m/s
  double time_0_60_s = 0.0;          // s
  double time_0_100_s = 0.0;         // s
  double brake_60_0_m = 0.0;         // m
  double lat_g_60 = 0.0;             // g
  double lat_g_120 = 0.0;            // g
  double displacement_cc = 0.0;      // engine displacement in cc (0 if electric/unknown)
  std::string aspiration;            // Naturally Aspirated / Turbo / Twin Turbo / Supercharged
};

struct PiResult {
  double raw_pi = 0.0;               // (0, 1]
  int class_id = 0;                  // 0..10 (F..X)
  int shown_pi = 0;                  // 1..999
  double lap_time_s = 0.0;           // best virtual lap time (s)
  double raw_lap_pi = 0.0;           // unanchored virtual-lap PI
  CarStats stats;
};

struct PiDiff {
  PiResult current;
  PiResult preview;
};

// Evaluates the virtual lap on `p` (optionally testing adjustable aero from `car`)
// and returns the minimum lap time in seconds (after drive-type lap scale).
double simulate_pi_lap(const drive::CarParams& p, const cars::CarPhysics* car, const PiConfig& cfg);

// Maps a virtual lap time to raw PI in [0, 1] (SPEC-GARAGE-UPGRADES).
double lap_time_to_pi(double lap_time_s, const PiConfig& cfg, double pi_floor = 0.0);

// Computes stat bars & vehicle performance metrics (power, torque, 0-60, top speed,
// braking distance, lateral G) directly from CarParams + CarPhysics.
CarStats compute_car_stats(const drive::CarParams& p, const cars::CarPhysics& car);

// Computes PI, class, shown PI, and stats for a car with `fitted_phys`.
// When `stock_phys` is provided and `stock_phys->has("Data_Car.PerformanceIndex")`,
// anchors the stock lap time to Data_Car.PerformanceIndex so stock cars match exactly
// and upgraded cars track the exact virtual-lap time delta.
PiResult compute_pi(const cars::CarPhysics& fitted_phys, const cars::PhysicsSettings& settings, const PiConfig& cfg,
                    const std::vector<ClassBand>& bands, const cars::CarPhysics* stock_phys = nullptr);

// Convenience wrapper querying `db` for stock and fitted CarPhysics.
bool compute_pi_for_car(const cars::PartsDb& db, int64_t car_id, const cars::FittedParts& fit,
                        const cars::PhysicsSettings& settings, const PiConfig& cfg,
                        const std::vector<ClassBand>& bands, PiResult& out, std::string* error = nullptr);

}  // namespace garage
