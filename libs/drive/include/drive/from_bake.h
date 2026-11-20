// CarParams from a baked car: the wheel geometry FORMAT-CARS gives (the
// body-frame wheel centres and the tyre sizes per axle); every other
// parameter stays at the synthetic car until apply_physics (from_physics.h).
#pragma once

#include "cars/bake_format.h"
#include "drive/vehicle.h"

namespace drive {

CarParams params_from_bake(const cars::bake::CarInfo& car);

// The model-frame matrix of the car body for the renderer (column-major,
// 16 floats): world from model = world from body x body from model, where
// body from model is FORMAT-CARS's pose (z mirrored, lifted, pitched).
void model_to_world(const cars::bake::CarInfo& car, const Vehicle& v, float out[16]);
// The same for a body frame at `position` with orientation `q` (a car
// placed without a Vehicle: traffic on its lane).
void model_to_world(const cars::bake::CarInfo& car, const Quat& q, const Vec3& position, float out[16]);

}  // namespace drive
