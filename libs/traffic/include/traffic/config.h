// Free-roam traffic's settings (SPEC-TRAFFIC): the `freeroam` block of
// `AIOpenWorld.xml` (the car list, groups and density lists) and the
// registry values of `GlobalRegistry.xml` (`AI/Spawning/...`,
// `AI/Traffic/...`), both in `media/gametunablesettings.zip`.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace traffic {

// `GlobalRegistry.xml`: `<Value id="a/b/c" type="float" value="..."/>`.
struct Registry {
  std::map<std::string, double> values;
  double get(std::string_view id, double fallback) const;
};

struct TrafficModel {
  int64_t id = 0;        // Data_Car.Id
  int max_active = 0;    // 0: no limit
  bool large = false;    // largevehicle
  bool stream_only = false;
};

struct DensityEntry {
  int id = 0;
  double min = 0, max = 0;
  double speed_mph = 0;            // 0: none given
  double spawn_in = 0, spawn_out = 0;  // m; 0: the registry's
  double away_chance = 0.6;
  std::set<int64_t> banned, allowed;  // model ids (allowed empty: any)
  bool allows(int64_t model) const {
    return !banned.count(model) && (allowed.empty() || allowed.count(model));
  }
};

struct TrafficConfig {
  int initial_cars = 4;
  int initial_festival_cars = 2;
  int max_loaded_models = 4;
  std::vector<TrafficModel> models;
  std::map<int, DensityEntry> traffic;   // cartype="traffic", by id
  std::map<int, DensityEntry> festival;  // cartype="festival", by id
  // The registry values traffic uses (SPEC-TRAFFIC; defaults are the disc's).
  double end_time_at_max = 17, start_time_at_min = 23, end_time_at_min = 4, start_time_at_max = 9;  // h
  double min_traffic = 8, max_traffic = 16;
  double min_festival = 2, max_festival = 7;
  double density_test_interval = 0.1, density_decay = 0.9;
  double spawn_in = 330, spawn_out = 370;
  double interval_min = 1, interval_med = 7, interval_max = 30, interval_variance = 2;  // s
  double interval_speed_factor = 2.0, interval_speed_max = 66.0;
  double min_gap = 12, max_gap = 30, speed_at_max_gap = 40;
  double away_chance_min = 0.25, away_chance_max = 0.65;
  double initial_min_distance = 60;
  int initial_max_cars = 6;
  double speed_min = 5, speed_max = 27, accel_max = 4, decel_max = 6;  // m/s, m/s^2
  double radius_for_min_speed = 1, radius_for_max_speed = 90, speed_variance = 0.01;
  double look_ahead_time = 1, min_look_ahead = 5, traffic_search = 30, player_search = 60;
  double model_rotation_time = 30, overspawn_check_time = 4;
  double high_lod_distance = 30, medium_lod_distance = 120;
  double fade_in_time = 1.0;              // AI/Spawning/FadeInTime (s)
  double min_full_alpha_distance = 150.0; // AI/Spawning/MinFullAlphaDistance (m)
  double max_full_alpha_distance = 320.0; // AI/Spawning/MaxFullAlphaDistance (m)
  double accel_max_input = 0.2, brake_max_input = 0.2;  // AI/Traffic/VehicleControls
  // AI/Traffic/Junctions & Avoidance
  double junction_max_collision_check_distance = 60.0;
  double junction_safe_time_gap = 2.0;
  bool junction_force_give_way = false;
  // AI/Traffic/LaneChangeBehavior
  double lane_change_chance = 0.10;
  double lane_change_rate = 0.20;
  double lane_change_min_interval = 5.0;
  double lane_change_time_blocked = 0.20;
  // AI/Traffic/CollisionAvoidance/Avoidance
  double avoid_max_offset = 5.0;
  double avoid_max_speed = 15.0;
  double avoid_max_distance = 60.0;
  // AI/Traffic/CollisionResponse
  double light_flash_chance = 0.70;
  double light_flash_time = 0.20;
  double time_between_light_flashes = 2.0;
  // AI/Traffic/DeadBehavior
  double dead_hazard_delay = 1.4;
  double dead_reanimate_delay = 2.0;
  double dead_reanimate_max_speed = 0.5;
  // AI/Traffic/VehicleControls (indicators)
  double min_distance_before_indicating = 20.0;
  double time_before_indicating = 2.5;
  double time_to_keep_indicating = 2.0;
  // AI/Traffic/VehiclePosition/Interpolation: the gains for a short car
  // (Min, at VehicleLengthAtMin) and a long one (Max, at VehicleLengthAtMax).
  struct LaneGains {
    double position_k = 0.999, heading_k = 0.95, lane_offset_k = 0.95;
  };
  LaneGains gains_min, gains_max{0.9, 0.93, 0.93};
  double gains_length_min = 4.5, gains_length_max = 10;  // m

  const DensityEntry* density(int id) const;
  const DensityEntry* festival_density(int id) const;
  void apply(const Registry& r);
};

// Parses the two XML documents (text). When `race_mode` is true, reads the
// `<Settings name="race">` block (falling back to `freeroam` if absent).
bool parse_traffic_config(std::string_view aiopenworld_xml, std::string_view registry_xml, TrafficConfig& out,
                          std::string* error, bool race_mode = false);
// Reads both from `<game>/media/gametunablesettings.zip`.
bool read_traffic_config(const std::filesystem::path& game_root, TrafficConfig& out, std::string* error,
                         bool race_mode = false);

// The time-of-day factor v in [0, 1] (SPEC-TRAFFIC, Time-of-day factor).
double time_factor(double hour, const TrafficConfig& c);
// A density entry's value at factor v: min + (max - min) v.
inline double density_at(const DensityEntry& e, double v) { return e.min + (e.max - e.min) * v; }
// The whole-world ceiling: MinTraffic + (MaxTraffic - MinTraffic) v.
inline double traffic_cap(const TrafficConfig& c, double v) { return c.min_traffic + (c.max_traffic - c.min_traffic) * v; }

}  // namespace traffic
