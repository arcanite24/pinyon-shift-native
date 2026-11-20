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
  int max_loaded_models = 4;
  std::vector<TrafficModel> models;
  std::map<int, DensityEntry> traffic;  // cartype="traffic", by id
  // The registry values traffic uses (SPEC-TRAFFIC; defaults are the disc's).
  double end_time_at_max = 17, start_time_at_min = 23, end_time_at_min = 4, start_time_at_max = 9;  // h
  double min_traffic = 8, max_traffic = 16;
  double density_test_interval = 0.1, density_decay = 0.9;
  double spawn_in = 330, spawn_out = 370;
  double interval_min = 1, interval_med = 7, interval_max = 30, interval_variance = 2;  // s
  double min_gap = 12, max_gap = 30, speed_at_max_gap = 40;
  double away_chance_min = 0.25, away_chance_max = 0.65;
  double initial_min_distance = 60;
  int initial_max_cars = 6;
  double speed_min = 5, speed_max = 27, accel_max = 4, decel_max = 6;  // m/s, m/s^2
  double radius_for_min_speed = 1, radius_for_max_speed = 90, speed_variance = 0.01;
  double look_ahead_time = 1, min_look_ahead = 5, traffic_search = 30, player_search = 60;
  double model_rotation_time = 30, overspawn_check_time = 4;
  double high_lod_distance = 30, medium_lod_distance = 120;

  const DensityEntry* density(int id) const;
  void apply(const Registry& r);
};

// Parses the two XML documents (text).
bool parse_traffic_config(std::string_view aiopenworld_xml, std::string_view registry_xml, TrafficConfig& out,
                          std::string* error);
// Reads both from `<game>/media/gametunablesettings.zip`.
bool read_traffic_config(const std::filesystem::path& game_root, TrafficConfig& out, std::string* error);

// The time-of-day factor v in [0, 1] (SPEC-TRAFFIC, Time-of-day factor).
double time_factor(double hour, const TrafficConfig& c);
// A density entry's value at factor v: min + (max - min) v.
inline double density_at(const DensityEntry& e, double v) { return e.min + (e.max - e.min) * v; }
// The whole-world ceiling: MinTraffic + (MaxTraffic - MinTraffic) v.
inline double traffic_cap(const TrafficConfig& c, double v) { return c.min_traffic + (c.max_traffic - c.min_traffic) * v; }

}  // namespace traffic
