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
#include <string_view>
#include <unordered_set>
#include <vector>

#include "drive/vehicle.h"
#include "fh1data/collision.h"

namespace world {

// Surface indices with a role of their own (COLLISION.md surface types).
constexpr uint8_t kSurfaceAsphalt = 0;
constexpr uint8_t kSurfaceInvisible = 9;         // a solid wall, kept
constexpr uint8_t kSurfaceCameraCollision = 49;  // the camera only, not the car

// Extended per-surface properties parsed from SurfaceTypes.xml.
struct SurfacePhysicsProps {
  std::string name;
  std::string category;
  float max_steering_lock_cap_deg = 0.0f;  // degrees (0 = uncapped)
  float linear_drag = 0.0f;                // <LinearDrag> (1/s)
  float angular_drag = 0.0f;               // <AngularDrag> (1/s)
  float chase_cam_spring_mult = 1.0f;      // <ChaseCamSpringMultiplier>
  float steer_torque = 1.0f;               // <SteerTorque>
  float car_world_coll_fric_mod = 1.0f;    // <CarWorldCollFricMods>
  float spring_norm_y_cutoff = 0.707f;     // <Springiness><NormDepSpringNormYCutoff>
  float spring_speed_cutoff = 4.0f;        // <Springiness><NormDepSpringSpeedCutoff>
  bool defined = false;
};

struct CategoryCollisionParams {
  std::string category_a;
  std::string category_b;
  float friction = 0.5f;
  float restitution = 0.0f;
  bool has_friction = false;
  bool has_restitution = false;
};

struct SurfaceTableExtras {
  std::vector<SurfacePhysicsProps> surfaces;
  std::map<std::string, float> car_world_coll_fric_mods;  // lowercase name/category -> multiplier
  std::vector<CategoryCollisionParams> category_params;

  const CategoryCollisionParams* find_category_params(std::string_view cat_a, std::string_view cat_b) const;
  float car_world_friction_mod(std::string_view surface_name, std::string_view category = {}) const;
};

// Parses extended SurfaceTypes.xml physics/camera/rumble/collision columns.
bool parse_surface_physics_extras(std::string_view xml, SurfaceTableExtras& out, std::string* error = nullptr);

struct CollisionMesh {
  std::vector<std::array<float, 3>> positions;  // three per triangle, game frame
  std::vector<uint8_t> surface;                 // one per triangle
  std::vector<uint8_t> flags;                   // one per triangle: the .fiz byte (not decoded)
  std::vector<uint32_t> tile;                   // one per triangle: N of N.fiz
  std::vector<uint16_t> word;                   // one per triangle: the .fiz u16 at 2 (not decoded)
  std::vector<uint32_t> edges;                  // one per triangle: the .fiz u32 at 28 (not decoded)
  std::vector<uint16_t> active;                 // one per triangle: its tile's mask for the active route
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
  const SurfaceTableExtras& surface_extras() const { return surface_extras_; }
  SurfacePhysicsProps surface_physics(int surface) const;
  drive::SurfaceTyre surface_tyre(int surface) const;
  const CategoryCollisionParams* category_collision_params(std::string_view cat_a, std::string_view cat_b) const {
    return surface_extras_.find_category_params(cat_a, cat_b);
  }
  std::string surface_name(int surface) const;
  // Engine choice until the tyre-model spec: grip = FrictionScale.
  float grip(int surface) const;
  // Whether the car collides with a surface (all but CameraCollision).
  static bool car_collides(int surface) { return surface != kSurfaceCameraCollision; }
  // Whether the car collides with a gathered triangle (COLLISION.md, Route
  // groups): car_collides, and the route word AND the triangle's tile mask
  // (0x8000, plus bit i when the active route is entry i of the tile's
  // route list) is not zero. The category byte is not applied yet.
  static bool collides(const CollisionMesh& cm, size_t t) {
    return car_collides(cm.surface[t]) && (t >= cm.word.size() || (cm.word[t] & cm.active[t]) != 0);
  }
  // The active route number R (free roam: 0; an event: its TrackRoute
  // number, probable). Applies to the next gather.
  void set_active_route(int route) {
    if (route != route_) cache_.clear();
    route_ = route;
  }
  int active_route() const { return route_; }

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
    std::vector<uint8_t> surface, flags;
    std::vector<uint16_t> word, active;
    std::vector<uint32_t> edges;
  };
  struct Impl;
  std::unique_ptr<Impl> impl_;
  int route_ = 0;
  fh1::ColIndex index_;
  std::unordered_set<uint32_t> occupied_cells_;
  std::vector<fh1::SurfaceType> surfaces_;
  SurfaceTableExtras surface_extras_;
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
