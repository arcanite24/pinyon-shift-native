// CarParams from a stock car's database rows and PhysicsSettings.ini
// (cars/physics.h), by SPEC-CAR-PHYSICS items 4 to 6. The wheel geometry
// comes from the bake (from_bake.h) when there is one, else from
// Data_CarBody and the tyre sizes (geometry_from_physics).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cars/bake_format.h"
#include "cars/physics.h"
#include "drive/vehicle.h"

namespace drive {

// The wheel centres and radii from Data_CarBody (ModelWheelbase, the outer
// tracks) and Data_Car's tyre sizes: the axles at z = +-wheelbase / 2, the
// wheels standing on y = 0 at their outer radius.
void geometry_from_physics(const cars::CarPhysics& car, CarParams& p);

// Everything but the geometry, which must already be in `p` (the centre of
// mass is placed between its axles). `notes` gets what had to fall back.
void apply_physics(const cars::CarPhysics& car, const cars::PhysicsSettings& settings, CarParams& p,
                   std::vector<std::string>* notes = nullptr);

// The sidewall factor for a tyre of this width (mm) and aspect
// (SPEC-CAR-PHYSICS, Curves: SidewallScalePeakSA).
double sidewall_stretch(double width_mm, double aspect, const cars::PhysicsSettings& settings);

// A baked car's parameters: the bake's geometry, then the physics rows
// carbake wrote beside it (<bake>/cars/<media name>.physics.txt) when they
// are there, or directly from the game disc (gamedb.slt + media/physics.zip)
// when set_disc_physics_root() has been configured. Without either, the car
// is the synthetic one; `notes` says so.
CarParams params_for_baked_car(const cars::bake::CarInfo& car, const std::filesystem::path& bake_dir,
                               std::vector<std::string>* notes = nullptr);

// Configures the fallback game disc root (and optional state root for v4/DLC)
// used for zero-bake in-memory physics loading when .physics.txt files are
// absent. Thread-safe.
void set_disc_physics_root(const std::filesystem::path& game_root,
                           const std::filesystem::path& state_root = {});

// Loads a stock car's CarPhysics rows and game-wide PhysicsSettings directly
// from the configured disc root (cached in memory per car). Thread-safe.
bool load_disc_car_physics(int64_t car_id, const std::string& media_name, cars::CarPhysics& out_car,
                           cars::PhysicsSettings& out_settings, std::string* error = nullptr);

// Loads the cached game-wide PhysicsSettings from media/physics.zip. Thread-safe.
bool load_disc_physics_settings(cars::PhysicsSettings& out_settings, std::string* error = nullptr);

// Player car-tuning sliders matching Career_Garage.Tuning_* (35 values).
// A negative value (default -1.0) means stock / unmodified; non-negative or
// signed alignment values apply when tuned.
struct CarTune {
  double front_tire_pressure_psi = -1.0;  // 15..55 PSI (default ~30)
  double rear_tire_pressure_psi = -1.0;
  double final_drive = -1.0;              // ratio (e.g. 2.2..6.1)
  std::array<double, 8> gears{-1, -1, -1, -1, -1, -1, -1, -1};  // ratios per gear 1..8
  // Alignment (degrees); sentinel < -90 means stock.
  double front_camber_deg = -100.0;       // -5.0..+5.0 deg
  double rear_camber_deg = -100.0;
  double front_toe_deg = -100.0;          // -5.0..+5.0 deg
  double rear_toe_deg = -100.0;
  double front_caster_deg = -100.0;       // 1.0..7.0 deg
  // Anti-roll bars, springs, ride height, damping: normalized [0..1] across DB Min..Max
  // (-1.0 = stock Def*).
  double front_arb = -1.0;
  double rear_arb = -1.0;
  double front_spring = -1.0;
  double rear_spring = -1.0;
  double front_ride_height = -1.0;        // [0..1] MinRideHeight..MaxRideHeight
  double rear_ride_height = -1.0;
  double front_rebound = -1.0;            // [0..1] MinDampenReboundRate..MaxDampenReboundRate
  double rear_rebound = -1.0;
  double front_bump = -1.0;               // [0..1] MinDampenBumpRate..MaxDampenBumpRate
  double rear_bump = -1.0;
  // Aero downforce sliders [0..1] (Drag0..Drag1, Downforce0..Downforce1).
  double front_downforce = -1.0;
  double rear_downforce = -1.0;
  // Brakes: balance [0..1] (0 = rear, 1 = front), pressure [0..2] (1.0 = 100%).
  double brake_balance = -1.0;
  double brake_pressure = -1.0;
  // Differential lock percentages [0..1] (0..100%), center split [0..1] (0 = front, 1 = rear).
  double front_diff_accel = -1.0;
  double front_diff_decel = -1.0;
  double rear_diff_accel = -1.0;
  double rear_diff_decel = -1.0;
  double center_diff_split = -1.0;

  bool is_default() const;
};

// Applies non-default CarTune sliders onto `p` using the bounds in `car`.
void apply_tune(const CarTune& tune, const cars::CarPhysics& car, CarParams& p);

// The same car with other rows (its fitted parts, SPEC-GARAGE-UPGRADES):
// the bake's geometry and the PhysicsSettings the bake wrote, `rows` for
// the rest. Without the bake's physics file, the synthetic car.
CarParams params_for_fitted_car(const cars::bake::CarInfo& car, const std::filesystem::path& bake_dir,
                                const cars::CarPhysics& rows, std::vector<std::string>* notes = nullptr,
                                const CarTune* tune = nullptr);

}  // namespace drive
