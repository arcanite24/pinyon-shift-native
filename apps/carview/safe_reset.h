// carview's reset (R, Start, and the headless driver's "stuck"): the car
// back on a road's centre line with room around it. Ours.
//
// The nav map's roads (SPEC-TRAFFIC; the same game frame as the drive) give
// the centre line. Candidates step along the nearest road from the point
// nearest the car, out to 60 m either way; the first whose ground lies
// within 2 m of the road's height, faces up, and has no wall within the
// car's clearance (a horizontal ray 0.7 m up, world::wall_between, towards
// eight points 3 m around it) wins. The car faces along the road, the way
// nearest its heading. A box centred past a thin wall is pushed out
// through it (the body contacts probe from the box centre), so a place
// with a wall inside the box is never taken.
#pragma once
#include <filesystem>
#include <memory>
#include <string>

#include "drive/vehicle.h"
#include "world/stream.h"

namespace traffic {
class Roads;
}

namespace carview {

class SafeReset {
 public:
  SafeReset();
  ~SafeReset();
  // Reads media/tracks/colorado/colorado.nav under `game_root`.
  bool open(const std::filesystem::path& game_root, std::string* error);
  // Places the car; false when no road within 60 m has a clear spot (the
  // caller falls back). `distance`: from the car to the spot (m).
  bool place(const world::Streamer& streamer, drive::Vehicle& vehicle, double* distance = nullptr) const;

 private:
  std::unique_ptr<traffic::Roads> roads_;
};

}  // namespace carview
