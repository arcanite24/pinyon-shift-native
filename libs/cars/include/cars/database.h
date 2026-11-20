// The car rows of the merged game database: v4's database with every
// installed package's merge database applied as whole rows by key, in
// package priority order (read-only; the merge happens in memory).
// Spec: FORMAT-CARS (DLC packages: database rows; Paint; Wheels; Brakes);
// AUTOSHOW_REFERENCE (The cars; Paint colour).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cars/sources.h"

namespace cars {

struct CarColour {
  int64_t id = 0;
  int64_t sequence = 0;
  bool metallic = false;
  uint32_t rgb = 0;  // r x 65536 + g x 256 + b, sRGB
  std::string base_texture, damage_texture;
};

struct CarRow {
  int64_t id = 0;
  std::string media_name;
  std::string display_name;
  int64_t stock_wheel_id = 0;
  std::string wheel_media;  // List_Wheels.MediaName of the stock wheel ("" if none)
  // Tyres per axle (0 front, 1 rear): width in mm, aspect, rim diameter in inches.
  std::array<int64_t, 2> tyre_width_mm{}, tyre_aspect{}, rim_inches{};
  std::array<double, 2> ride_height{};
  uint32_t caliper_rgb_front = 0, caliper_rgb_rear = 0;
  double caliper_angle_front = 0, caliper_angle_rear = 0;
  bool selectable = false, drivable = false, unicorn = false;
  bool has_livery = false;
  int64_t wing_mask = 0;
  std::vector<CarColour> colours;  // by Sequence, then ID
  // CarPartPositions PartId 0-9 in the model's frame (+X right, +Z rear);
  // 5 left rear, 6 right rear, 7 right front, 8 left front.
  std::array<std::array<double, 3>, 10> part_positions{};
  std::array<bool, 10> has_part_position{};
  int part_position_duplicates = 0;  // identical duplicate rows left by the merge
  std::string origin;  // "base", "v4" or the package id whose merge adds the row
};

struct Database {
  std::vector<CarRow> cars;  // by Id
  std::vector<std::string> merges_applied;  // in order, for the report
  size_t base_rows = 0, v4_rows = 0;
};

// Reads the disc's media/db/gamedb.slt (to tell base rows from v4 rows), v4's
// db/gamedb.slt from install.v4_media_zip (or the disc's database when there
// is no v4) and every package's merge databases.
bool load_database(const Install& install, Database& out, std::string* error);

}  // namespace cars
