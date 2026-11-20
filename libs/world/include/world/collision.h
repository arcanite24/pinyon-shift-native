// Colorado's collision (COLLISION.md): the `N.fiz` tiles of bin.zip around
// a point, placed by the `.col` lattice, with the surface table of
// physics.zip. Everything is in the game frame, the drive's world frame.
//
// Loading rule (COLLISION.md "load around a point"): the tiles whose lattice
// cell comes within the radius, each contributing its core triangles (those
// whose centroid lies in the tile's own 100 m cell), so the 8 m aprons the
// neighbours repeat are dropped. Parsed tiles are kept, so a gather after
// the car has moved only reads the new ones.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "drive/vehicle.h"
#include "fh1data/collision.h"

namespace world {

// Surface indices with a role of their own (COLLISION.md surface types).
constexpr uint8_t kSurfaceAsphalt = 0;
constexpr uint8_t kSurfaceInvisible = 9;         // a solid wall, kept
constexpr uint8_t kSurfaceCameraCollision = 49;  // the camera only, not the car

struct CollisionMesh {
  std::vector<std::array<float, 3>> positions;  // three per triangle, game frame
  std::vector<uint8_t> surface;                 // one per triangle
  size_t tiles = 0;
  size_t triangles() const { return surface.size(); }
};

class Collision {
 public:
  Collision();
  ~Collision();
  // `colorado`: the folder with bin.zip and Ribbon_00/Colorado_track_00.col.
  // `physics_zip`: media/physics.zip (surfaceTypes.xml); empty or missing:
  // every surface gets grip 1, logged in `warning`.
  bool open(const std::filesystem::path& colorado, const std::filesystem::path& physics_zip, std::string* error,
            std::string* warning = nullptr);

  const fh1::ColIndex& index() const { return index_; }
  const std::vector<fh1::SurfaceType>& surfaces() const { return surfaces_; }
  std::string surface_name(int surface) const;
  // Engine choice until the tyre-model spec: grip = FrictionScale.
  float grip(int surface) const;
  // Whether the car collides with a surface (all but CameraCollision).
  static bool car_collides(int surface) { return surface != kSurfaceCameraCollision; }

  // The core triangles of every tile whose cell comes within `radius` of
  // (x, z).
  bool gather(double x, double z, double radius, CollisionMesh& out, std::string* error);
  // Reads one tile (by its N) without the cache.
  bool read_tile(uint32_t tile, fh1::FizTile& out, std::string* error);
  // The lattice cell (i, j) of record `r`, and whether a point lies in it.
  bool in_cell(const fh1::ColRecord& r, double x, double z) const;

 private:
  struct Core {
    std::vector<std::array<float, 3>> positions;
    std::vector<uint8_t> surface;
  };
  struct Impl;
  std::unique_ptr<Impl> impl_;
  fh1::ColIndex index_;
  std::vector<fh1::SurfaceType> surfaces_;
  std::map<uint32_t, Core> cache_;  // by record position
};

// The measured free-roam spawn (COLLISION.md spawn point): the body
// reference point at rest and the forward direction, game frame.
constexpr double kSpawnX = 2002.96, kSpawnY = 143.84, kSpawnZ = 960.00;
constexpr double kSpawnForwardX = 0.8072, kSpawnForwardZ = -0.5884;
// Its heading on the drive's convention (0 faces +z, positive turns towards
// +x): 126.1 degrees.
double spawn_heading();

// Casts down at (x, z) from `from_y` and returns the ground height under
// it, or false.
bool ground_height(const drive::Ground& ground, double x, double z, double from_y, double& y,
                   drive::Hit* hit = nullptr);

}  // namespace world
