// Free-roam collectibles in Colorado: the 100 discount signs and the 9 barn
// finds, their placements, the player's progress, and the rules that find
// them each simulation step. Also the upgrade discount and the map
// projection they feed.
// Spec: SPEC-FREEROAM-COLLECTIBLES (Discount signs; Barn finds; The minimal
// map: Background texture and projection).
//
// Barn finds follow the title's controller: a spawn roll every
// TimeBetweenTicks, a pending barn's rumour after MinTimeBeforeSpawn of
// driving, discovery, restoration over a driven distance, and collection at
// the garage (see FreeRoam). Interim, marked where they apply: a sign breaks
// when the car body touches it above 1 m/s; found signs and a discovered
// barn's car are not drawn; the discount applies to upgrades only.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace freeroam {

// A GameObjs.xml object: position (game frame, m) and its three axes.
struct Placement {
  double x = 0, y = 0, z = 0;
  std::array<double, 3> x_axis{1, 0, 0}, y_axis{0, 1, 0}, z_axis{0, 0, 1};
};

struct Sign {
  std::string id;  // lower case GameplayID, flyer_001 ...
  Placement at;
};

// One <RadioDJRequests> line of an activity (SPEC-RADIO): a RadioSystem.xml
// gameplayEvent for a station's special (or immediate) dialogue, with its
// delay window in seconds.
struct RadioRequest {
  bool immediate = false;
  std::string event;
  double delay_min_s = 0, delay_max_s = 0;
  bool enabled = true, top_priority = false;
};

struct Barn {
  std::string name;     // barnfind_01 ...
  std::string trigger;  // lower case GameplayID of the derelict car (TriggerZone object)
  Placement at;         // the trigger object
  double radius = 10;   // m
  double max_mph = 50;
  // The rumour circle (HintRegion): centre = trigger + (x_offset east,
  // y_offset north), radii in m.
  double hint_x = 0, hint_z = 0, hint_rx = 0, hint_rz = 0;
  // DoorInfo, lower case GameplayIDs.
  std::string open_id, closed_id, open_coll_id, closed_coll_id;
  int car_id = 0;            // UnlockCar id (Data_Car)
  std::string car_name_key;  // UnlockCar stringTableNameId
  // SpawnInfo: the pick weight and the 1-in-N odds of a spawn roll, from
  // min_probability at min_distance to max_probability at max_distance of
  // total odometer (the file's miles, here in metres).
  int spawn_weighting = 100;
  double spawn_min_probability = 1, spawn_max_probability = 1;
  double spawn_min_distance_m = 0, spawn_max_distance_m = 0;
  // DistanceDriven: the restoration distance after discovery, from
  // distanceMax (found at 0) down to distanceMin (found at threshold or
  // more of odometer); metres.
  double restore_min_m = 0, restore_max_m = 0, restore_threshold_m = 0;
  // BarnFindLocationHint<n> (enabled, no delay) and BarnFindSpotted<n>
  // (disabled, 600-1200 s); n is the barn's own, not its activity number.
  std::vector<RadioRequest> radio_requests;
  // The rumour's reveal (the activity's MapAnimation and MapVO substitute
  // states): the map profile's camera animation (barn_find_region<k>_unlock)
  // and the VO line Dak plays as the map opens (BarnFind<n>AppearsOnMap).
  std::string map_animation, map_vo;
};

struct Collectibles {
  std::vector<Sign> signs;  // in Flyers.xml order
  std::vector<Barn> barns;  // in barnfinds.xml order
};

// Reads Colorado/Flyers.xml and Colorado/barnfinds.xml from
// media/gamemodes.zip and the placements from
// media/tracks/colorado/Ribbon_00/GameObjs.xml under `game_root`.
bool load_collectibles(const std::filesystem::path& game_root, Collectibles& out, std::string* error);
// The same from the three files' texts. Activities whose object is not
// placed are an error.
bool parse_collectibles(std::string_view flyers_xml, std::string_view barns_xml, std::string_view game_objs_xml,
                        Collectibles& out, std::string* error);

// Barn statuses (the profile record's u32, the title's values).
enum BarnStatus : int {
  kBarnReady = 0,       // restored: collect it at the garage
  kBarnRestoring = 1,   // discovered, being restored
  kBarnCollected = 2,   // in the garage
  kBarnNotSpawned = 3,
  kBarnPending = 4,     // spawned, its rumour not started yet
  kBarnRumour = 5,
};
// A barn the player has driven up to (restoring, ready or collected).
inline bool barn_discovered(int status) {
  return status == kBarnRestoring || status == kBarnReady || status == kBarnCollected;
}
inline bool barn_status_known(int status) { return status >= kBarnReady && status <= kBarnRumour; }

struct BarnProgress {
  int status = kBarnNotSpawned;
  double rumour_at = 0;  // total distance when the rumour started (m, 0 before)
  double found_at = 0;   // total distance when discovered (m, 0 before)
};

// The distance a barn found at `found_at` (m of odometer) needs to be
// driven before it is restored: max + (min - max) * clamp(found / threshold).
double barn_restore_distance(const Barn& b, double found_at);

// The player's progress, ours to save (drive::DriveSave carries it).
struct Progress {
  double total_distance = 0;  // m driven, ever
  std::set<std::string> signs_found;
  std::vector<BarnProgress> barns;  // by Collectibles::barns index (sized on use)
  std::vector<int> garage;          // Data_Car ids given by barn finds
};

// The garage's COLLECT CAR on a ready barn (status 0): status 2 and its car
// into Progress::garage. False if the barn is not ready.
bool collect_barn(const Collectibles& data, Progress& progress, size_t index);

// What the barn controller needs besides the car (the title's predicates):
// free roam (not a race or a menu), no activity blocking barn finds (PR
// stunts, speed traps and cameras), and no dialogue playing.
struct BarnContext {
  bool free_roam = true;
  bool blocked = false;
  bool dialogue = false;
};
// The interim break speed (m/s).
constexpr double kSignBreakSpeed = 1.0;
// How far the sign's own collision reaches past its placement point (m),
// kept for backward compatibility; actual billboard board extents use the
// oriented half-width and half-depth below (matching the ~4.1 m wide sign).
constexpr double kSignReach = 0.6;
constexpr double kSignBoardHalfWidth = 2.05;
constexpr double kSignBoardHalfDepth = 0.45;
constexpr double kSignHeightTolerance = 3.5;

// The car for one step: where it is, which way it faces, its body's half
// width and half length, its speed, and how far it moved.
struct CarState {
  double x = 0, y = 0, z = 0;
  double forward_x = 0, forward_z = 1;  // unit, horizontal
  double half_width = 0.9, half_length = 2.2;
  double speed = 0;  // m/s
  double moved = 0;  // m this step
  double dt = 0;     // s this step (the barn timers; 0 leaves them still)
};

struct Event {
  // kBarnFound: discovered, restoration starts; kBarnReady: restored.
  enum Kind { kSignSmashed, kRumourStarted, kBarnFound, kBarnReady } kind;
  int index;  // into Collectibles::signs or ::barns
};

class FreeRoam {
 public:
  FreeRoam(const Collectibles& data, Progress& progress);
  // The barn tuning (GlobalRegistry Activities/BarnFinds; the disc's values
  // by default) and the random source of the spawn roll and the weighted
  // pick (uniform in [0, 1]; a seeded generator by default).
  void set_barn_tuning(double time_between_ticks_s, double max_idle_time_s, double min_distance_before_next_m,
                       double min_speed_before_spawn_ms, double min_time_before_spawn_s);
  void set_random(std::function<double()> uniform01) { uniform_ = std::move(uniform01); }
  // One simulation step while collectibles are live.
  std::vector<Event> step(const CarState& car, const BarnContext& ctx = {});
  // The garage's COLLECT CAR on a ready barn: status 2 and its car into
  // Progress::garage. False if the barn is not ready.
  bool collect_barn(size_t index);
  // The props not to draw (lower case GameplayIDs): found signs, the open
  // doors of a barn not yet found, the closed doors and the car of one
  // found. Rebuilt when progress changes.
  std::shared_ptr<const std::set<std::string>> hidden() const { return hidden_; }
  int signs_found() const { return int(progress_.signs_found.size()); }
  int barns_found() const;  // discovered (restoring, ready or collected)
  const Collectibles& data() const { return data_; }
  const Progress& progress() const { return progress_; }

 private:
  void rebuild_hidden();
  const Collectibles& data_;
  Progress& progress_;
  std::vector<char> sign_found_;
  std::shared_ptr<const std::set<std::string>> hidden_;
  void step_barns(const CarState& car, const BarnContext& ctx, std::vector<Event>& events, bool& changed);
  double uniform();
  // Tuning (seconds, metres, m/s).
  double tick_s_ = 300, max_idle_s_ = 600, min_gap_m_ = 48280.32, min_speed_ = 8.9408, min_time_s_ = 15;
  double tick_clock_ = 0;   // since the last spawn roll
  double idle_s_ = 0;       // stationary time (the idle check; interim, SPEC Q116)
  double pending_s_ = 0;    // the pending barn's rumour timer
  std::function<double()> uniform_;
  std::mt19937 rng_{0x5eed1u};
};

// Main/UpgradeDiscount from the found count: one percent a sign.
inline int upgrade_discount(int signs_found) { return signs_found < 0 ? 0 : signs_found > 100 ? 100 : signs_found; }
// An upgrade's price with the discount (The reward): floor((1 - d) p + 0.5)
// in single precision when d > 0.
int64_t discounted_price(int64_t price, int discount_percent);

// The map background's projection: u 0 west to 1 east, v 0 north to 1
// south, from the game frame's X and Z (+Z north).
constexpr double kMapWest = -9583.3, kMapWidth = 19166.7, kMapNorth = 4600.0, kMapHeight = 11500.0;
std::array<double, 2> map_uv(double x, double z);

}  // namespace freeroam
