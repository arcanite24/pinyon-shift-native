// A route's AI line (SPEC-RACE-AI, The AI line): aiopenworld.zip members
// colorado/Ribbon_00/route_NNN.owt, big-endian. A 32-byte header (magic
// OWTM, version 1, the point count, a closed flag), n 48-byte points (the
// centre, a lateral vector whose length is the road's half width and which
// points left, an offset in half widths) and a 16-byte trailer (the magic
// and version again). The racing line is centre + offset x lateral.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace race {

struct AiLinePoint {
  float x = 0, y = 0, z = 0;     // the road's centre (game frame)
  float lx = 0, ly = 0, lz = 0;  // to the left, |l| = the half width
  float offset = 0;              // the racing line, in half widths (left positive)

  float half_width() const;
  // The racing line's point: centre + offset x lateral.
  float race_x() const { return x + offset * lx; }
  float race_y() const { return y + offset * ly; }
  float race_z() const { return z + offset * lz; }
};

struct AiLineFile {
  uint32_t version = 0;
  bool closed = false;
  std::vector<AiLinePoint> points;
};

// Decodes one file; the size must be exactly header + points + trailer.
bool parse_owt(std::span<const uint8_t> bytes, AiLineFile& out, std::string* error);
// Reads route `route_id`'s line from <game_root>/media/aiopenworld.zip.
bool load_owt(const std::filesystem::path& game_root, int route_id, AiLineFile& out, std::string* error);
// Every member name ending .owt in the archive (the census).
std::vector<std::string> owt_members(const std::filesystem::path& game_root, std::string* error);

// The polyline's length (closed lines include the closing segment).
double centre_length(const AiLineFile& line);
double racing_length(const AiLineFile& line);

}  // namespace race
