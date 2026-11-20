#include "drive/from_bake.h"

#include <cmath>

namespace drive {

CarParams params_from_bake(const cars::bake::CarInfo& car) {
  CarParams p;
  for (int i = 0; i < kWheels; ++i) {
    const auto& c = car.wheel_centres_body[size_t(i)];
    p.wheel_centres[size_t(i)] = {c[0], c[1], c[2]};
    // The tyre's outer radius stands the wheel on y = 0 (FORMAT-CARS).
    p.tyre_radius[size_t(i)] = c[1] > 0.05f ? c[1] : 0.3;
  }
  const double z_front = (p.wheel_centres[0].z + p.wheel_centres[1].z) / 2;
  const double z_rear = (p.wheel_centres[2].z + p.wheel_centres[3].z) / 2;
  p.centre_of_mass.z = (z_front + z_rear) / 2;  // placeholder: even split
  const double track = std::fabs(p.wheel_centres[1].x - p.wheel_centres[0].x);
  if (track > 0.5) p.size.x = track + 0.2;
  if (std::fabs(z_front - z_rear) > 1) p.size.z = std::fabs(z_front - z_rear) + 1.6;
  return p;
}

void model_to_world(const cars::bake::CarInfo& car, const Vehicle& v, float out[16]) {
  // body = B model + (0, lift, 0), B = [[1,0,0],[0,c,s],[0,s,-c]].
  const double c = std::cos(car.pitch), s = std::sin(car.pitch);
  const double b[3][3] = {{1, 0, 0}, {0, c, s}, {0, s, -c}};
  const Quat q = v.orientation();
  const Vec3 cols[3] = {rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, 1})};
  // world = R body + p  =>  world = (R B) model + R (0, lift, 0) + p.
  for (int col = 0; col < 3; ++col) {
    Vec3 r{};
    for (int k = 0; k < 3; ++k) {
      r.x += cols[k].x * b[k][col];
      r.y += cols[k].y * b[k][col];
      r.z += cols[k].z * b[k][col];
    }
    out[col * 4 + 0] = float(r.x);
    out[col * 4 + 1] = float(r.y);
    out[col * 4 + 2] = float(r.z);
    out[col * 4 + 3] = 0;
  }
  const Vec3 p = v.position();
  out[12] = float(cols[1].x * car.lift + p.x);
  out[13] = float(cols[1].y * car.lift + p.y);
  out[14] = float(cols[1].z * car.lift + p.z);
  out[15] = 1;
}

}  // namespace drive
