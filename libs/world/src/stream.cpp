#include "world/stream.h"

#include <cmath>

namespace world {

float placeholder_grip(Surface s) {
  switch (s) {
    case Surface::kRoad: return 1.0f;
    case Surface::kTerrain: return 0.75f;
    case Surface::kWater: return 0.5f;
    case Surface::kOther: return 0.9f;
    case Surface::kProp: return 0.9f;
  }
  return 1.0f;
}

bool Streamer::update(double x, double z, bool* changed, std::string* error) {
  if (changed) *changed = false;
  if (gathers_ > 0 && std::hypot(x - cx_, z - cz_) <= regather_) return true;
  Mesh mesh;
  GatherStats stats;
  if (!world_.gather(x, z, options_, mesh, &stats, error)) return false;
  // The ground: the world-space triangles world::ground_object keeps.
  std::vector<std::array<float, 3>> positions;
  std::vector<float> grip;
  positions.reserve(mesh.positions.size());
  grip.reserve(mesh.triangles());
  for (size_t t = 0; t < mesh.triangles(); ++t) {
    if (!mesh.ground[t]) continue;
    positions.push_back(mesh.positions[t * 3]);
    positions.push_back(mesh.positions[t * 3 + 1]);
    positions.push_back(mesh.positions[t * 3 + 2]);
    grip.push_back(placeholder_grip(mesh.surface[t]));
  }
  ground_ = std::make_unique<drive::MeshGround>(std::move(positions), std::move(grip));
  mesh_ = std::move(mesh);
  stats_ = std::move(stats);
  cx_ = x;
  cz_ = z;
  ++gathers_;
  if (changed) *changed = true;
  return true;
}

}  // namespace world
