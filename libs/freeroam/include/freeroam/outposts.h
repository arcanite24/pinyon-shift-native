// Horizon Festival Outposts and Outpost PR Stunts (Speed, Skill/Drift, Photo Op).
//
// Loaded from Colorado's game data:
// - media/gamemodes.zip: Colorado/gas_stations.xml, mission_speedstunts.xml,
//   mission_prstunts.xml, mission_photoshoots.xml
// - media/tracks/colorado/Ribbon_00/GameObjs.xml (GASSTATION_XXX, OUTPOST_XXX_NODE,
//   speed_camera_XX_left / _right)
// - media/tracks/colorado/Ribbon_00/TrackRouteXXX.xml (mission_photo_end_01,
//   mission_start_01)
// - media/stringtables/EN.zip: GasStation.str, Missions.str, List_SpeedCameras.str
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "freeroam/collectibles.h"
#include "freeroam/tunables.h"

namespace fh1 {
struct StringTable;
}

namespace freeroam {

struct Outpost {
  std::string id;            // e.g. "GasStation_01"
  int index = 0;             // 1..10
  std::string name_key;      // e.g. "IDS_GasStationName_01"
  std::string display_name;  // localized name from GasStation.str (or fallback)
  Placement trigger;         // GASSTATION_XXX
  double trigger_radius = 50.0;
  Placement discovery;       // GASSTATION_XXX_DISCOVERY (fallback: trigger)
  double discovery_radius = 150.0;
  Placement spawn;           // OUTPOST_XXX_NODE (fast-travel / hub exit pose)
  std::vector<std::string> mission_ids;  // linked PR stunts
};

enum class PrStuntKind {
  kSpeed = 0,
  kSkill = 1,
  kPhoto = 2,
};

struct PrStunt {
  std::string id;            // e.g. "speedstunt_01", "prstunt_01", "photoshoot_01"
  PrStuntKind kind = PrStuntKind::kSpeed;
  std::string outpost_id;    // parent Outpost id (e.g. "GasStation_01")
  std::string title;         // short display title
  std::string instruction;   // HUD objective text
  Placement start_pos;       // mission_start_01 or parent outpost spawn
  Placement target_pos;      // speed trap midpoint / skill zone / photo landmark
  Placement gate_left;       // speed trap left post (kSpeed)
  Placement gate_right;      // speed trap right post (kSpeed)
  bool has_gate = false;
  double target_mph = 0.0;      // kSpeed (easy/default)
  int target_points = 0;        // kSkill (easy/default)
  double photo_radius = 50.0;   // kPhoto
  double time_limit_s = 120.0;  // 0 = untimed (easy/default)
  // Retail XML difficulty tiers ([0]=easy, [1]=medium, [2]=hard), fallback to easy when absent.
  std::array<double, 3> target_mph_tiers{};
  std::array<int, 3> target_points_tiers{};
  std::array<double, 3> time_limit_tiers{};
  int route_id = 0;
  int loaner_car_id = 0;
  int loaner_car_colour = 0;
  // The title pays nothing for a PR stunt (first or repeat): completing
  // one only raises the outpost's fast-travel discount.
  int64_t reward_credits = 0;
  int64_t reward_popularity = 0;
};

struct OutpostCatalog {
  std::vector<Outpost> outposts;
  std::vector<PrStunt> stunts;

  const Outpost* find_outpost(std::string_view id) const;
  const PrStunt* find_stunt(std::string_view id) const;
  std::vector<const PrStunt*> stunts_for_outpost(std::string_view outpost_id) const;
};

// Pure parser for unit tests and load_outposts().
bool parse_outposts(std::string_view gas_stations_xml, std::string_view speed_stunts_xml,
                    std::string_view pr_stunts_xml, std::string_view photo_shoots_xml,
                    std::string_view game_objs_xml, const std::map<int, std::string>& route_xmls,
                    const fh1::StringTable* gas_strings, const fh1::StringTable* mission_strings,
                    const fh1::StringTable* camera_strings, OutpostCatalog& out, std::string* error);

// Loads Colorado's Festival Outposts and Outpost PR Stunts from `game_root`.
bool load_outposts(const std::filesystem::path& game_root, OutpostCatalog& out, std::string* error);

// Persistent discovery & completion state for Outposts and PR Stunts.
struct OutpostProgress {
  std::set<std::string> discovered_outposts;
  std::set<std::string> completed_pr_stunts;

  bool is_outpost_discovered(std::string_view id) const;
  bool is_stunt_completed(std::string_view id) const;
  bool discover_outpost(const std::string& id);
  bool complete_stunt(const std::string& id);

  // Checks if `(car_x, car_z)` enters any undiscovered Outpost's discovery zone.
  // Returns the index in `catalog.outposts` if newly discovered, else -1.
  int check_discovery(const OutpostCatalog& catalog, double car_x, double car_z);

  int discovered_count(const OutpostCatalog& catalog) const;
  int completed_stunt_count(const OutpostCatalog& catalog) const;

  // Caches outpost -> stunt IDs from `catalog` so `is_outpost_fast_travel_free(int)`
  // can evaluate custom or retail outpost mission rosters.
  void bind_catalog(const OutpostCatalog& catalog);

  // True once all PR Stunts (3/3 in retail Colorado) at the outpost are completed,
  // unlocking free fast travel to that Outpost.
  bool is_outpost_fast_travel_free(int outpost_id) const;
  bool is_outpost_fast_travel_free(const OutpostCatalog& catalog, int outpost_id) const;
  bool is_outpost_fast_travel_free(const OutpostCatalog& catalog, std::string_view outpost_id) const;

  // The outpost's PR stunts, and how many of them are completed.
  int stunts_at(const OutpostCatalog& catalog, std::string_view outpost_id) const;
  int completed_stunts_at(const OutpostCatalog& catalog, std::string_view outpost_id) const;
  // The price of a fast travel to the outpost (freeroam::fast_travel_cost
  // over its completed stunts); -1 for an unknown outpost.
  int64_t fast_travel_cost(const OutpostCatalog& catalog, std::string_view outpost_id,
                           const FastTravelTuning& tuning) const;

 private:
  std::map<int, std::vector<std::string>> outpost_stunts_by_index_;
};

enum class PrStuntState {
  kInactive = 0,
  kActive = 1,
  kSucceeded = 2,
  kFailed = 3,
};

struct PrStuntUpdate {
  double car_x = 0.0;
  double car_z = 0.0;
  double prev_x = 0.0;
  double prev_z = 0.0;
  double speed_mph = 0.0;
  int64_t total_skill_points = 0;  // banked skill points (or banked + unbanked if unbanked_skill_points is set)
  bool photo_pressed = false;
  double dt = 0.0;

  // R3-CAM-12: Explicit banked skill points (when >= 0, Skill Stunt counts ONLY
  // banked points earned inside the stunt zone; when < 0, falls back to
  // total_skill_points - unbanked_skill_points).
  int64_t banked_skill_points = -1;
  int64_t unbanked_skill_points = 0;

  // R3-CAM-12: Optional camera pose for Photo Op frustum validation (both player car
  // and target landmark must project inside the camera view frustum in front of camera).
  double car_y = 0.0;
  bool has_camera = false;
  std::array<double, 3> camera_pos{};
  std::array<double, 3> camera_forward{0.0, 0.0, 1.0};
  std::array<double, 3> camera_right{1.0, 0.0, 0.0};
  std::array<double, 3> camera_up{0.0, 1.0, 0.0};
  double camera_vfov_deg = 55.0;
  double camera_aspect = 16.0 / 9.0;
};

// Live runtime tracker for an active Outpost PR Stunt.
class PrStuntSession {
 public:
  void start(const PrStunt& stunt, int64_t baseline_skill_points = 0, int difficulty_index = 0);
  void cancel();
  PrStuntState update(const PrStuntUpdate& u);

  static bool point_in_camera_frustum(const std::array<double, 3>& point, const std::array<double, 3>& cam_pos,
                                      const std::array<double, 3>& cam_fwd, const std::array<double, 3>& cam_right,
                                      const std::array<double, 3>& cam_up, double vfov_deg, double aspect,
                                      double radius_m = 0.0, double max_range_m = 250.0);

  bool active() const { return state_ == PrStuntState::kActive; }
  PrStuntState state() const { return state_; }
  const PrStunt& stunt() const { return stunt_; }
  double time_remaining_s() const { return time_remaining_s_; }
  double elapsed_s() const { return elapsed_s_; }
  double best_mph() const { return best_mph_; }
  int64_t earned_skill_points() const { return earned_skill_points_; }
  double distance_to_target() const { return distance_to_target_; }
  bool in_target_zone() const { return in_target_zone_; }
  bool photo_framed() const { return photo_framed_; }
  double progress_fraction() const;
  std::string status_line() const;

 private:
  PrStunt stunt_;
  PrStuntState state_ = PrStuntState::kInactive;
  int64_t baseline_skill_points_ = 0;
  int64_t last_banked_skill_points_ = 0;
  int64_t earned_skill_points_ = 0;
  double time_remaining_s_ = 0.0;
  double elapsed_s_ = 0.0;
  double best_mph_ = 0.0;
  double distance_to_target_ = 0.0;
  double photo_hold_s_ = 0.0;
  bool in_target_zone_ = false;
  bool photo_framed_ = false;
};

const char* pr_stunt_kind_name(PrStuntKind kind);

}  // namespace freeroam
