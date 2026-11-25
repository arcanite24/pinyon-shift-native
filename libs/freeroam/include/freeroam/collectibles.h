// Free-roam collectibles in Colorado: the 100 discount signs and the 9 barn
// finds, their placements, the player's progress, and the rules that find
// them each simulation step. Also the upgrade discount and the map
// projection they feed.
// Spec: SPEC-FREEROAM-COLLECTIBLES (Discount signs; Barn finds; The minimal
// map: Background texture and projection).
//
// Interim rules from the spec's Unknowns, all marked where they apply:
// a sign breaks when the car body touches it above 1 m/s; the next barn's
// rumour starts every 50 km of total distance, lowest activity number
// first; the distance is in metres; found signs and a collected barn car
// are not drawn; the discount applies to upgrades only.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
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

// Barn statuses (the profile record's u32; Barn finds, States).
enum BarnStatus : int { kBarnFound = 2, kBarnNotSpawned = 3, kBarnRumour = 5 };

struct BarnProgress {
  int status = kBarnNotSpawned;
  double rumour_at = 0;  // total distance when the rumour started (m, 0 before)
  double found_at = 0;   // total distance when discovered (m, 0 before)
};

// The player's progress, ours to save (drive::DriveSave carries it).
struct Progress {
  double total_distance = 0;  // m driven, ever
  std::set<std::string> signs_found;
  std::vector<BarnProgress> barns;  // by Collectibles::barns index (sized on use)
  std::vector<int> garage;          // Data_Car ids given by barn finds
};

// Fallback rumour spacing (m of total distance) when no campaign progression
// context is supplied (backwards-compatible with standalone free-roam tests).
constexpr double kRumourSpacing = 50000;

// Campaign progression context for gating Barn Find rumours naturally across
// wristbands, completed events, and cruising distance.
struct BarnProgressionContext {
  int wristband_level = 0;   // 0=Yellow .. 6=Gold
  int events_completed = 0;  // total career events completed/won
  bool active = false;       // true -> use progression-gated rumour rules
};

// Per-barn progression gate thresholds for Colorado's 9 Barn Finds.
struct BarnRumourGate {
  int min_wristband = 0;
  int min_events = 0;
  double min_distance_since_last_m = 8000.0;
};

BarnRumourGate barn_rumour_gate(size_t barn_index);
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
};

struct Event {
  enum Kind { kSignSmashed, kRumourStarted, kBarnFound } kind;
  int index;  // into Collectibles::signs or ::barns
};

class FreeRoam {
 public:
  FreeRoam(const Collectibles& data, Progress& progress);
  // Supplies current campaign progression so step(car) gates Barn Find rumours
  // by wristband level, completed events, and cruising distance.
  void set_progression(int wristband_level, int events_completed);
  // One simulation step while collectibles are live (free roam).
  std::vector<Event> step(const CarState& car);
  std::vector<Event> step(const CarState& car, const BarnProgressionContext& progression);
  // The props not to draw (lower case GameplayIDs): found signs, the open
  // doors of a barn not yet found, the closed doors and the car of one
  // found. Rebuilt when progress changes.
  std::shared_ptr<const std::set<std::string>> hidden() const { return hidden_; }
  int signs_found() const { return int(progress_.signs_found.size()); }
  int barns_found() const;
  const Collectibles& data() const { return data_; }
  const Progress& progress() const { return progress_; }

 private:
  void rebuild_hidden();
  const Collectibles& data_;
  Progress& progress_;
  std::vector<char> sign_found_;
  std::shared_ptr<const std::set<std::string>> hidden_;
  BarnProgressionContext progression_;
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
