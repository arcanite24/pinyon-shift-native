// The free-roam values of the game's tunable registry:
// media/gametunablesettings.zip GlobalRegistry.xml,
// `<Value id="Activities/Outposts/TotalCost" type="int" value="10000"/>`.
// The fast-travel price of an outpost and the barn-find tuning read from it.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace freeroam {

// The registry's numeric values by id.
struct Registry {
  std::map<std::string, double> values;
  bool has(std::string_view id) const { return values.count(std::string(id)) > 0; }
  double get(std::string_view id, double fallback) const;
};

bool parse_registry(std::string_view xml, Registry& out, std::string* error);
bool load_registry(const std::filesystem::path& game_root, Registry& out, std::string* error);

// Activities/Outposts: the price of a fast travel to an outpost (defaults
// are the disc's values).
struct FastTravelTuning {
  int64_t total_cost = 10000;                       // TotalCost
  std::array<double, 4> discount{0.0, 0.25, 0.5, 1.0};  // Discount_<n>_Missions
  static FastTravelTuning from(const Registry& r);
};

// The title's rule: free once every PR stunt of the outpost is done;
// otherwise TotalCost less trunc(TotalCost * Discount_<done>), never below
// 0 (10,000 / 7,500 / 5,000 CR on the disc). The festival and the garages
// are always free (not priced here).
int64_t fast_travel_cost(const FastTravelTuning& t, int stunts_done, int stunts_total);

// Activities/BarnFinds (defaults are the disc's values).
struct BarnFindTuning {
  double max_idle_time_s = 600.0;                 // MaxIdleTime: no tick after this long idle
  double min_distance_before_next_m = 48280.32;   // MinDistanceBeforeNextSpawn (30 mi)
  double min_speed_before_spawn_ms = 8.9408;      // MinSpeedBeforeSpawn (20 mph)
  double min_time_before_spawn_s = 15.0;          // MinTimeBeforeSpawn
  double time_between_ticks_s = 300.0;            // TimeBetweenTicks
  double time_prompt_on_screen_s = 8.0;           // TimePromptOnScreen
  static BarnFindTuning from(const Registry& r);
};

// HeroCar: the car the first-time career hands over (the Corrado VR6 in its
// colour sequence 2, upgraded to C 400 on the workshop's locked map, Q280).
// upgrades: Career_Garage column -> Level (HeroCar/Upgrades/<column>, the
// zero levels left out); wheel_style: List_Wheels Id (0 keeps the stock).
struct HeroCarPreset {
  int64_t car_id = 309;
  int colour_index = 2;
  int64_t wheel_style = 640;
  std::map<std::string, int> upgrades;
  static HeroCarPreset disc();  // the disc's GlobalRegistry values
  static HeroCarPreset from(const Registry& r);
};

}  // namespace freeroam
