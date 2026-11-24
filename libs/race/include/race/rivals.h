// Clean-room Rivals & Time-Attack ghost recording, interpolation, `.ghost`
// persistence, clean-lap personal-best tracking, and Rally Rivals loaner car
// IDs (SPEC-V4-CONTENT: event-only loaner twins 1626..1635).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace race {

// The 10 Rally Rivals loaner twin car IDs (Data_Car.Id 1626..1635, SPEC-V4-CONTENT).
constexpr std::array<int64_t, 10> rally_rivals_loaner_ids() {
  return {1626, 1627, 1628, 1629, 1630, 1631, 1632, 1633, 1634, 1635};
}
constexpr bool is_rally_rivals_loaner(int64_t car_id) { return car_id >= 1626 && car_id <= 1635; }

struct GhostSample {
  double time = 0.0;        // elapsed race/lap time (s)
  double x = 0.0, y = 0.0, z = 0.0;
  double qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0;  // unit orientation quaternion
  double speed = 0.0;       // forward speed (m/s)
  float steer = 0.0f;       // front wheel steer angle (rad)
  float brake = 0.0f;       // brake lamp input [0, 1]
  int gear = 1;             // -1 reverse, 0 neutral, 1..n forward
};

struct GhostRun {
  std::string event_id;       // e.g. "FR22" or "stage:18"
  std::string driver_name;    // player or rival display name
  std::string car_media;      // Data_Car.MediaName
  int64_t car_id = 0;         // Data_Car.Id
  int shown_pi = 0;           // car shown PI at recording time
  double total_time = 0.0;    // finish or lap time (s)
  bool clean = true;          // true = certified clean lap (no rewind / dirty flag)
  double sample_interval = 1.0 / 30.0;  // recording cadence (s)
  std::vector<GhostSample> samples;

  void clear();
  bool empty() const { return samples.empty(); }
  double duration() const;

  // Appends a sample when empty, `force == true`, or `s.time >= samples.back().time + sample_interval`.
  void record(const GhostSample& s, bool force = false);

  // Evaluates an interpolated ghost pose at elapsed time `t` (linear position/speed/steer,
  // shortest-path quaternion slerp). Returns false when empty.
  bool interpolate(double t, GhostSample& out) const;

  // Clean-lap preference comparator: a certified clean run always beats a dirty run;
  // among equal cleanliness, a strictly lower positive `total_time` wins.
  static bool is_better(const GhostRun& candidate, const GhostRun& current_best);

  // `.ghost` text serialization (`shift-ghost 1`).
  std::string to_text() const;
  static bool from_text(std::string_view text, GhostRun& out, std::string* error = nullptr);
  bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
  static bool load(const std::filesystem::path& path, GhostRun& out, std::string* error = nullptr);
};

// Returns `<ghost_dir>/<sanitized_event_id>.ghost`.
std::filesystem::path ghost_file_path(const std::filesystem::path& ghost_dir, std::string_view event_id);

// Compares `candidate` against any existing personal-best `.ghost` in `ghost_dir`
// (preferring clean laps over dirty laps) and saves it when `candidate` is better.
// Returns true on success (whether or not the previous PB was beaten); sets `*saved_new_best` if updated.
bool update_personal_best_ghost(const std::filesystem::path& ghost_dir, const GhostRun& candidate,
                                bool* saved_new_best = nullptr, std::string* error = nullptr);

}  // namespace race
