// The racing line a race's cars share, from the route's AI line
// (SPEC-RACE-AI, The AI line: centre + offset x lateral), as a
// drive::RacingLine with the curve radius (from the points 4 either side),
// the height and the room to each road edge per point. A closed line is
// unrolled into consecutive laps, so progress along it only grows and a
// gap between two cars never jumps at the line's seam.
// Also the ribbon ground: the road surface the line describes (centre +-
// lateral), for cars beyond the gathered collision.
#pragma once

#include <vector>

#include "drive/ai_driver.h"
#include "drive/vehicle.h"
#include "race/owt.h"
#include "race/route.h"

namespace race {

struct RaceLine {
  drive::RacingLine line;
  bool closed = false;
  double lap_length = 0;  // one lap of a closed line; the whole line if open
  // A closed line: the progress of the start line on the second copy; the
  // grid stands behind it (negative race distance). Open: the start line's
  // progress (or 0).
  double gate_progress = 0;
};

// `copies`: how many laps a closed line is unrolled into (laps + 3 leaves
// one before the grid and two after the finish).
RaceLine build_race_line(const AiLineFile& file, const Route* route, int copies);

// The road the AI line describes: quads from each point to the next,
// centre +- lateral, widened by `margin` m each side (a car running wide
// keeps ground under it), asphalt (grip 1). Raycasts look up a uniform
// grid of cells.
class RibbonGround : public drive::Ground {
 public:
  explicit RibbonGround(const AiLineFile& file, double margin = 8, double cell = 24);
  bool raycast(const drive::Vec3& origin, const drive::Vec3& dir, double max_distance, drive::Hit& hit) const override;
  size_t triangles() const { return tri_.size() / 9; }

 private:
  std::vector<float> tri_;  // 9 floats a triangle
  std::vector<std::vector<uint32_t>> cells_;
  double x0_ = 0, z0_ = 0, cell_ = 24;
  int nx_ = 0, nz_ = 0;
};

}  // namespace race
