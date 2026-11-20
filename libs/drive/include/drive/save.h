// The engine's own save for the drive (ours, M4 groundwork): where the car
// stands and which car it is, so a restart puts the player back. A small
// versioned text file of our own format, kept in a user-data folder of
// ours; it is not the game's profile and reads nothing of it.
//
//   shift-drive-save 1
//   car NIS_SkylineGTRVSpecII_02
//   position -1790.46 34.16 1863.44
//   heading -1.4358
//   spawn 2002.96 143.84 960.00 2.2008
//
// Lines are "key values"; unknown keys are skipped, so later versions can
// add lines. `spawn` (optional) is the last spawn point, where "back to the
// start" goes when no road is found.
#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace drive {

struct DriveSave {
  static constexpr int kVersion = 1;
  std::string car;
  std::array<double, 3> position{};
  double heading = 0;  // rad, 0 faces +z, positive turns towards +x
  bool has_spawn = false;
  std::array<double, 3> spawn{};
  double spawn_heading = 0;
};

std::string save_to_text(const DriveSave& save);
// False with `error` on a missing header, a newer version, or a missing
// car, position or heading.
bool save_from_text(const std::string& text, DriveSave& out, std::string* error);

// Reads `path`; false with an empty `error` when the file does not exist.
bool read_save(const std::filesystem::path& path, DriveSave& out, std::string* error);
// Writes through a temporary file renamed over `path`, so a crash leaves
// the old save or the new one; creates the folder.
bool write_save(const std::filesystem::path& path, const DriveSave& save, std::string* error);

}  // namespace drive
