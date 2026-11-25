// A route file (SPEC-EVENTS, Route files): media/tracks/colorado/Ribbon_00/
// TrackRoute<NNN>.xml, named transforms with a position (game frame) and a
// horizontal facing, optionally a width or a tag. The grid slots, the start
// line (the circuit's lap gate) and the sprint finish come from it.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace race {

struct RouteTransform {
  std::string name;
  double x = 0, y = 0, z = 0;
  double fx = 0, fz = 1;  // facing (horizontal)
  std::optional<double> width;
  std::string tag;
};

// A vertical plane (x, z) with its horizontal unit normal: a gate crossed
// forwards along the normal.
struct Gate {
  double x = 0, z = 0, nx = 0, nz = 1;
  double reach = 1e30;  // a crossing counts within this distance of (x, z)
  bool both_ways = false;  // a backward crossing counts too (checkpoints)
  double side(double px, double pz) const { return (px - x) * nx + (pz - z) * nz; }
  // A forward crossing on the move from (x0, z0) to (x1, z1): the fraction
  // of the move at the plane, or a negative number.
  double crossing(double x0, double z0, double x1, double z1) const;
};

struct Route {
  int id = 0;
  std::vector<RouteTransform> transforms;

  const RouteTransform* find(std::string_view name) const;
  // start_location_00, _01, ... in order (00 is pole), until one is missing.
  std::vector<const RouteTransform*> slots() const;
  // route_waypoint_00, _01, ... in order, until one is missing.
  std::vector<const RouteTransform*> waypoints() const;
  // The plane through start_gantry_cannon_00 normal to the slots' facing
  // (slot 00's), counted within 60 m of the gantry.
  std::optional<Gate> start_line() const;
  // The plane through the midpoint of end_race_cannon_left_04 and _right_04,
  // normal to their facing, moved 3.5 m forward along it.
  std::optional<Gate> sprint_finish() const;
};

bool parse_route(std::string_view text, Route& out, std::string* error);
std::filesystem::path route_path(const std::filesystem::path& game_root, int route_id);
bool load_route(const std::filesystem::path& game_root, int route_id, Route& out, std::string* error);
// A route file by its path (another world's Ribbon_00: ColoradoDirt).
bool load_route_file(const std::filesystem::path& path, int route_id, Route& out, std::string* error);

}  // namespace race
