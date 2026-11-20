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
// are there. Without them the car is the synthetic one; `notes` says so.
CarParams params_for_baked_car(const cars::bake::CarInfo& car, const std::filesystem::path& bake_dir,
                               std::vector<std::string>* notes = nullptr);

}  // namespace drive
