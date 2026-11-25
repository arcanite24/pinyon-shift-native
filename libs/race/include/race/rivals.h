// Clean-room Rivals & Time-Attack ghost recording, interpolation, `.shgh`
// persistence, clean-lap personal-best tracking, offline Bronze/Silver/Gold
// target rivals & bounties, and Rally Rivals loaner car IDs (SPEC-V4-CONTENT:
// event-only loaner twins 1626..1635).
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

struct RivalTarget {
  int tier_index = 1;        // 1 = Bronze, 2 = Silver, 3 = Gold
  std::string tier_label;    // "BRONZE", "SILVER", "GOLD"
  std::string rival_name;    // offline target rival display name
  double target_time_s = 0.0;
  int64_t bounty_cr = 0;
};

struct GhostWaypoint {
  double progress = 0.0;
  double x = 0.0, y = 0.0, z = 0.0;
};

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

// Returns `<ghost_dir>/<sanitized_event_id>.shgh`.
std::filesystem::path ghost_file_path(const std::filesystem::path& ghost_dir, std::string_view event_id);

// Loads an existing personal-best ghost for `event_id` (checks `.shgh` first,
// falling back to legacy `.ghost` if present).
bool load_personal_best_ghost(const std::filesystem::path& ghost_dir, std::string_view event_id, GhostRun& out,
                              std::string* error = nullptr);

// Compares `candidate` against any existing personal-best `.shgh` in `ghost_dir`
// (preferring clean laps over dirty laps) and saves it when `candidate` is better.
// Returns true on success (whether or not the previous PB was beaten); sets `*saved_new_best` if updated.
bool update_personal_best_ghost(const std::filesystem::path& ghost_dir, const GhostRun& candidate,
                                bool* saved_new_best = nullptr, std::string* error = nullptr);

// Generates 3 offline Bronze / Silver / Gold Rivals targets for an event or Rally stage.
std::array<RivalTarget, 3> generate_rivals_targets(std::string_view event_id, double base_time_s,
                                                   int64_t base_prize_cr, bool is_rally_stage = false);

// Synthesizes a smooth `.shgh`-compatible target ghost along ordered 3D route waypoints
// finishing in `target_time_s`.
GhostRun synthesize_target_ghost(const std::vector<GhostWaypoint>& waypoints, double target_time_s,
                                 std::string_view event_id, std::string_view rival_name,
                                 std::string_view car_media, int64_t car_id = 0, int shown_pi = 500,
                                 double sample_interval = 0.1);

// Evaluates a completed Rivals run against `target_time_s`. Sets `beat_rival` when
// `player_time_s` is strictly faster than `target_time_s` and returns the CR bounty
// (100% of `base_bounty_cr` for clean runs, 75% for dirty runs, 0 on loss).
int64_t evaluate_rivals_bounty(double player_time_s, bool clean, double target_time_s, int64_t base_bounty_cr,
                               bool& beat_rival);

// Formats a Rivals/Time-Trial duration as MM:SS.mmm (or "--:--.---" if <= 0).
std::string format_rivals_time(double seconds);

}  // namespace race
