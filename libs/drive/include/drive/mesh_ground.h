// A static triangle mesh as ground: a bounding-volume hierarchy over the
// triangles, raycast by the suspension like any drive::Ground.
//
// M3 stand-in: until COLLISION.md says what the game collides with, the
// engine drives on its render triangles (libs/world builds them). The mesh
// is just another Ground, so the real collision source replaces it without
// touching the vehicle. The grip per triangle is ours too (a placeholder
// per surface class), until the spec names surfaces.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "drive/vehicle.h"

namespace drive {

class MeshGround : public Ground {
 public:
  // `positions`: three corners per triangle, in the drive's world frame.
  // `grip`: one factor per triangle (empty: 1 for every triangle).
  // `surface`: one index per triangle, reported in Hit::surface (empty: -1).
  MeshGround(std::vector<std::array<float, 3>> positions, std::vector<float> grip = {},
             std::vector<uint8_t> surface = {});

  // The nearest hit along the ray within `max_distance`, either face. The
  // normal faces against the ray.
  bool raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const override;
  // The tyre columns by surface index, reported in Hit::tyre (empty, or an
  // index past the end: the defaults).
  void set_surface_tyres(std::vector<SurfaceTyre> tyres) { tyres_ = std::move(tyres); }

  size_t triangle_count() const { return positions_.size() / 3; }
  size_t node_count() const { return nodes_.size(); }
  int depth() const { return depth_; }

 private:
  struct Node {
    std::array<float, 3> lo, hi;
    uint32_t first = 0, count = 0;  // leaf: triangles [first, first + count) of order_
    uint32_t left = 0, right = 0;   // inner (count == 0): children
  };
  uint32_t build(uint32_t first, uint32_t count, int depth);

  std::vector<std::array<float, 3>> positions_;
  std::vector<float> grip_;
  std::vector<uint8_t> surface_;
  std::vector<SurfaceTyre> tyres_;
  std::vector<uint32_t> order_;
  std::vector<std::array<float, 3>> centroid_;
  std::vector<Node> nodes_;
  int depth_ = 0;
};

}  // namespace drive
