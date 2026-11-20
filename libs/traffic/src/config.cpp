#include "traffic/config.h"

#include <cmath>
#include <cstdlib>

#include "audio/xml.h"
#include "fh1data/archive.h"

namespace traffic {

double Registry::get(std::string_view id, double fallback) const {
  const auto it = values.find(std::string(id));
  return it == values.end() ? fallback : it->second;
}

const DensityEntry* TrafficConfig::density(int id) const {
  const auto it = traffic.find(id);
  return it == traffic.end() ? nullptr : &it->second;
}

void TrafficConfig::apply(const Registry& r) {
  auto g = [&](const char* id, double& v) { v = r.get(id, v); };
  g("AI/Spawning/Density/EndTimeAtMaxDensity", end_time_at_max);
  g("AI/Spawning/Density/StartTimeAtMinDensity", start_time_at_min);
  g("AI/Spawning/Density/EndTimeAtMinDensity", end_time_at_min);
  g("AI/Spawning/Density/StartTimeAtMaxDensity", start_time_at_max);
  g("AI/Spawning/Density/MinTraffic", min_traffic);
  g("AI/Spawning/Density/MaxTraffic", max_traffic);
  g("AI/Spawning/Density/DensityTestInterval", density_test_interval);
  g("AI/Spawning/Density/DensityDecay", density_decay);
  g("AI/Spawning/SpawnInDistance", spawn_in);
  g("AI/Spawning/SpawnOutDistance", spawn_out);
  g("AI/Spawning/Interval/TrafficMin", interval_min);
  g("AI/Spawning/Interval/TrafficMed", interval_med);
  g("AI/Spawning/Interval/TrafficMax", interval_max);
  g("AI/Spawning/Interval/TrafficVariance", interval_variance);
  g("AI/Spawning/MinDistanceFromOtherCars", min_gap);
  g("AI/Spawning/MaxDistanceFromOtherCars", max_gap);
  g("AI/Spawning/SpeedAtMaxDistanceFromOtherCars", speed_at_max_gap);
  g("AI/Spawning/SpawnAwayChanceMin", away_chance_min);
  g("AI/Spawning/SpawnAwayChanceMax", away_chance_max);
  g("AI/Spawning/InitialSpawnMinDistance", initial_min_distance);
  double imc = initial_max_cars;
  g("AI/Spawning/InitialSpawnMaxCars", imc);
  initial_max_cars = int(imc);
  g("AI/Traffic/Speed/SpeedMin", speed_min);
  g("AI/Traffic/Speed/SpeedMax", speed_max);
  g("AI/Traffic/Speed/AccelerationMax", accel_max);
  g("AI/Traffic/Speed/DeccelerationMax", decel_max);
  g("AI/Traffic/Speed/RadiusForMinSpeed", radius_for_min_speed);
  g("AI/Traffic/Speed/RadiusForMaxSpeed", radius_for_max_speed);
  g("AI/Traffic/Speed/SpeedVariance", speed_variance);
  g("AI/Traffic/CollisionAvoidance/LookAheadTime", look_ahead_time);
  g("AI/Traffic/CollisionAvoidance/MinLookAheadDistance", min_look_ahead);
  g("AI/Traffic/CollisionAvoidance/TrafficCollisionSearchDistance", traffic_search);
  g("AI/Traffic/CollisionAvoidance/PlayerCollisionSearchDistance", player_search);
  g("AI/Traffic/VehicleManager/ModelRotationTime", model_rotation_time);
  g("AI/Traffic/VehicleManager/OverspawnCheckTime", overspawn_check_time);
  g("AI/Traffic/VehiclePosition/MaxHighLodDistance", high_lod_distance);
  g("AI/Traffic/VehiclePosition/MaxMediumLodDistance", medium_lod_distance);
  g("AI/Traffic/VehicleControls/AccelMaxInput", accel_max_input);
  g("AI/Traffic/VehicleControls/BrakeMaxInput", brake_max_input);
  g("AI/Traffic/VehiclePosition/Interpolation/Min/PositionK", gains_min.position_k);
  g("AI/Traffic/VehiclePosition/Interpolation/Min/HeadingK", gains_min.heading_k);
  g("AI/Traffic/VehiclePosition/Interpolation/Min/LaneOffsetK", gains_min.lane_offset_k);
  g("AI/Traffic/VehiclePosition/Interpolation/Max/PositionK", gains_max.position_k);
  g("AI/Traffic/VehiclePosition/Interpolation/Max/HeadingK", gains_max.heading_k);
  g("AI/Traffic/VehiclePosition/Interpolation/Max/LaneOffsetK", gains_max.lane_offset_k);
  g("AI/Traffic/VehiclePosition/Interpolation/VehicleLengthAtMin", gains_length_min);
  g("AI/Traffic/VehiclePosition/Interpolation/VehicleLengthAtMax", gains_length_max);
}

namespace {

int64_t to_int(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

void group_models(const audio::XmlNode* list, const std::map<std::string, std::set<int64_t>>& groups,
                  std::set<int64_t>& out) {
  if (!list) return;
  for (const auto& g : list->children) {
    if (audio::iequals(g.name, "Group")) {
      const auto it = groups.find(g.attr("id"));
      if (it != groups.end()) out.insert(it->second.begin(), it->second.end());
    } else if (audio::iequals(g.name, "Car")) {
      out.insert(to_int(g.attr("model")));
    }
  }
}

}  // namespace

bool parse_traffic_config(std::string_view aiopenworld_xml, std::string_view registry_xml, TrafficConfig& out,
                          std::string* error) {
  audio::XmlNode ai, reg;
  if (!audio::parse_xml(aiopenworld_xml, ai, error)) return false;
  if (!audio::parse_xml(registry_xml, reg, error)) return false;
  const audio::XmlNode* settings = nullptr;
  for (const auto& c : ai.children)
    if (audio::iequals(c.name, "Settings") && audio::iequals(c.attr("name"), "freeroam")) settings = &c;
  if (!settings) {
    if (error) *error = "AIOpenWorld.xml: no freeroam settings";
    return false;
  }
  const audio::XmlNode* list = settings->child("CarList");
  if (!list) {
    if (error) *error = "AIOpenWorld.xml: no freeroam CarList";
    return false;
  }
  out.initial_cars = int(list->number("numInitialTrafficCars", out.initial_cars));
  out.max_loaded_models = int(list->number("maxLoadedTrafficModels", out.max_loaded_models));
  out.models.clear();
  for (const auto& c : list->children) {
    if (!audio::iequals(c.name, "Car")) continue;
    TrafficModel m;
    m.id = to_int(c.attr("model"));
    m.max_active = int(c.number("maxactive", 0));
    m.large = audio::iequals(c.attr("largevehicle"), "true");
    m.stream_only = audio::iequals(c.attr("streamonly"), "true");
    out.models.push_back(m);
  }
  std::map<std::string, std::set<int64_t>> groups;
  if (const auto* g = settings->find("Densities/CarGroups"))
    for (const auto& grp : g->children)
      if (audio::iequals(grp.name, "Group"))
        for (const auto& car : grp.children) groups[grp.attr("id")].insert(to_int(car.attr("model")));
  out.traffic.clear();
  if (const auto* dens = settings->child("Densities"))
    for (const auto& dl : dens->children) {
      if (!audio::iequals(dl.name, "DensityList") || !audio::iequals(dl.attr("cartype"), "traffic")) continue;
      for (const auto& d : dl.children) {
        if (!audio::iequals(d.name, "Density")) continue;
        DensityEntry e;
        e.id = int(d.number("id", 0));
        e.min = d.number("min", 0);
        e.max = d.number("max", 0);
        e.speed_mph = d.number("speed", 0);
        e.spawn_in = d.number("spawnInDistance", 0);
        e.spawn_out = d.number("spawnOutDistance", 0);
        e.away_chance = d.number("spawnAwayChance", e.away_chance);
        group_models(d.child("BannedVehicles"), groups, e.banned);
        group_models(d.child("AllowedVehicles"), groups, e.allowed);
        out.traffic[e.id] = e;
      }
    }
  Registry r;
  for (const auto& v : reg.children)
    if (audio::iequals(v.name, "Value") && v.has("id")) r.values[v.attr("id")] = v.number("value", 0);
  out.apply(r);
  return true;
}

bool read_traffic_config(const std::filesystem::path& game_root, TrafficConfig& out, std::string* error) {
  const auto zip = game_root / "media" / "gametunablesettings.zip";
  auto a = fh1::Archive::open(zip, error);
  if (!a) return false;
  auto member = [&](std::string_view name, std::string& text) {
    auto r = a->read(name);
    if (!r.ok()) {
      if (error) *error = zip.string() + ": " + std::string(name) + ": " + r.error;
      return false;
    }
    text.assign(r.data.begin(), r.data.end());
    return true;
  };
  std::string ai, reg;
  if (!member("AIOpenWorld.xml", ai) || !member("GlobalRegistry.xml", reg)) return false;
  return parse_traffic_config(ai, reg, out, error);
}

double time_factor(double hour, const TrafficConfig& c) {
  double h1 = c.end_time_at_max, h2 = c.start_time_at_min, h3 = c.end_time_at_min, h4 = c.start_time_at_max;
  if (h2 < h1) h2 += 24;
  if (h3 < h2) h3 += 24;
  if (h4 < h3) h4 += 24;
  double h = std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0);
  if (h < h1) h += 24;
  double f;
  if (h < h2) f = 1 - (h - h1) / (h2 - h1);
  else if (h < h3) f = 0;
  else if (h <= h4) f = (h - h3) / (h4 - h3);
  else f = 1;
  return (1 - std::cos(3.14159265358979323846 * f)) / 2;
}

}  // namespace traffic
