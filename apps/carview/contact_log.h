// carview --contacts-log: a diagnostic of the car's hard body contacts
// against the collision (COLLISION.md .fiz triangles) and of the walls in
// the collision that nothing drawn stands near ("invisible walls").
//
// Ours, debugging only. DrawnIndex buckets the gathered scenery's drawn
// triangles (world::Mesh: world-space models, props, the far ring, tree
// sprites as upright boxes; upright triangles only, |normal y| < 0.7: the
// road and terrain under a wall are not what shows it) in a 4 m x-z grid so the nearest drawn surface
// within a metre of a point is cheap to find.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "world/collision.h"
#include "world/world.h"

namespace drive {
class Ground;
}

namespace carview {

class DrawnIndex {
 public:
  // upright: index only upright triangles and sprites (else every one).
  void build(const world::Mesh& mesh, bool upright = true);
  // The heights where a vertical line at (x, z) meets indexed triangles
  // (not sprites), highest first.
  std::vector<double> heights(double x, double z) const;
  // The distance from p to the nearest drawn triangle (or tree sprite)
  // within `reach`, and that triangle; -1 when none is that near.
  double nearest(const std::array<float, 3>& p, double reach, uint32_t* triangle = nullptr) const;
  size_t triangles() const { return count_; }

 private:
  static int64_t key(int i, int j) { return (int64_t(i) << 32) ^ uint32_t(j); }
  const world::Mesh* mesh_ = nullptr;
  std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
  std::vector<uint8_t> sprite_;  // per triangle: a tree sprite
  size_t count_ = 0;
};

// The car-height part of the collision's walls with nothing drawn within a
// metre, grouped by tile, surface and flags byte. Each steep (|normal y| <
// 0.5), car-colliding triangle within `radius` (x-z) of a point is sampled
// every half metre; a sample is at car height when the ground (`ground`,
// probed straight down beside the wall) is 0.1 to 1.5 m below it, and bare
// when nothing drawn lies within `reach`. `samples`, `bare`: car-height
// samples checked and bare, every group together. Triangles as
// world::Collision::collides takes them.
struct UndrawnWalls {
  uint32_t tile = 0;
  int surface = 0, flags = 0, word = 0;
  uint32_t edges = 0;
  size_t triangles = 0, samples = 0, bare = 0;  // bare samples: half a metre each
  std::array<double, 3> lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};  // the bare samples' box
};
std::vector<UndrawnWalls> undrawn_walls(const world::CollisionMesh& cm, const drive::Ground& ground,
                                        const DrawnIndex& drawn, double x, double z, double radius, double reach,
                                        size_t* samples, size_t* bare);

}  // namespace carview
