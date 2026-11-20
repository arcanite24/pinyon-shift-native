// traffic-census: SPEC-TRAFFIC's oracle census over our traffic, headless.
// The player stands at the oracle's parked spot (2002.9, 143.8, 960.0) on
// a density-5 road: 60 s at noon, 60 s at 02:00 (counted from 30 s after
// the change), then 30 s driving along the road at 10.4 m/s (313 m). Each
// half second it counts the active traffic within 1 km and checks every
// car against the road map: within 15 m of a traffic road, on two-way
// roads right of the centre line at 2.2 +- 0.5 m. It also reports the
// spawn and despawn distances, the share driving away, and the speeds on
// straight road. The population is kinematic here (no bodies): the
// physics level of detail is the host's (bodies.h).
//
//   traffic-census --game-root <disc> [--seed N] [--seeds N] [--verbose]
//
// Exit 0 when every tolerance of SPEC-TRAFFIC holds over the seeds' mean,
// 1 when one misses (each miss is printed), 2 on a usage or read error.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "traffic/config.h"
#include "traffic/nav.h"
#include "traffic/population.h"
#include "traffic/roads.h"

namespace {

struct Phase {
  double sum = 0;
  int samples = 0, min = 1 << 30, max = 0;
  void add(int n) {
    sum += n, samples++;
    min = std::min(min, n), max = std::max(max, n);
  }
  double mean() const { return samples ? sum / samples : 0; }
};

struct Result {
  Phase noon, night, cruise;
  int car_samples = 0, off_road = 0, two_way = 0, right = 0;
  std::vector<double> offsets, straight_speeds;
  traffic::PopulationStats stats;
};

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

Result run(const traffic::Roads& roads, const traffic::TrafficConfig& config, uint64_t seed, bool verbose) {
  Result out;
  traffic::Population pop(roads, config, seed);
  traffic::P3 player{2002.9, 143.8, 960.0}, velocity{0, 0, 0};
  // The cruise: along the player's road, the way its right-hand lane runs.
  const auto start = roads.nearest(player.x, player.z, true);
  traffic::Car walker;
  walker.road = start.road;
  walker.s = start.s;
  walker.dir = start.lateral < 0 ? -1 : 1;
  constexpr double kDt = 1.0 / 60, kCruise = 313.0 / 30;
  const int steps = int(150 / kDt);
  for (int i = 0; i <= steps; ++i) {
    const double t = i * kDt;
    pop.set_hour(t < 60 ? 12 : t < 120 ? 2 : 12);
    if (t >= 120) {
      // Drive along the road (its centre line at the parked offset).
      walker.s += walker.dir * kCruise * kDt;
      std::array<double, 2> d;
      const auto p = roads.point_at(walker.road, walker.s, nullptr, &d);
      const auto rt = traffic::right_of(d[0] * walker.dir, d[1] * walker.dir);
      player = {p.x + rt[0] * 2.2, p.y, p.z + rt[1] * 2.2};
      velocity = {d[0] * walker.dir * kCruise, 0, d[1] * walker.dir * kCruise};
    }
    pop.step(kDt, player, velocity);
    if (i % 30 != 0) continue;
    int active = 0;
    for (const auto& c : pop.cars()) {
      if (std::hypot(c.position.x - player.x, c.position.z - player.z) > 1000) continue;
      ++active;
      if (c.speed < 1) continue;  // moving samples
      out.car_samples++;
      const auto h = roads.nearest(c.position.x, c.position.z, true, 60);
      if (h.road < 0 || h.distance > 15) {
        out.off_road++;
        continue;
      }
      const auto& r = roads.roads()[size_t(c.road)];
      if (!r.oneway) {
        // Its offset right of the centre line, in its direction of travel.
        out.two_way++;
        std::array<double, 2> d;
        const auto p = roads.point_at(c.road, c.s, nullptr, &d);
        const auto rt = traffic::right_of(d[0] * c.dir, d[1] * c.dir);
        const double lat = (c.position.x - p.x) * rt[0] + (c.position.z - p.z) * rt[1];
        out.right += lat > 0;
        out.offsets.push_back(lat);
      }
      // Straight road: no bend under 300 m within 60 m either way.
      bool straight = true;
      for (size_t k = 0; k < r.points.size(); ++k)
        if (std::fabs(r.s[k] - c.s) < 60 && r.radius[k] < 300) straight = false;
      if (straight && c.age > 5) out.straight_speeds.push_back(c.speed);
    }
    if (t < 60) out.noon.add(active);
    else if (t >= 90 && t < 120) out.night.add(active);
    else if (t >= 120) out.cruise.add(active);
    if (verbose && i % 1200 == 0 && t > 0)
      for (const auto& c : pop.cars())
        std::printf("    car %u model %lld road %d dir %+d s %.1f v %.1f target %.1f d %.0f phys %d\n", c.id,
                    (long long)c.model, c.road, c.dir, c.s, c.speed, c.target,
                    std::hypot(c.position.x - player.x, c.position.z - player.z), int(c.physics));
    if (verbose && i % 600 == 0)
      std::printf("  t %5.1f  h %4.1f  D %.2f  target %.2f  active %d  loaded %zu\n", t, pop.hour(),
                  pop.local_density(), pop.target(), active, pop.loaded().size());
  }
  out.stats = pop.stats();
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path game;
  uint64_t seed = 1;
  int seeds = 10;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game-root") game = next();
    else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--seeds") seeds = std::max(1, std::atoi(next().c_str()));
    else if (a == "--verbose") verbose = true;
    else {
      std::fprintf(stderr, "usage: traffic-census --game-root <disc> [--seed N] [--seeds N] [--verbose]\n");
      return 2;
    }
  }
  if (game.empty())
    if (const char* g = std::getenv("SHIFT_GAME_ROOT")) game = g;
  if (game.empty()) {
    std::fprintf(stderr, "traffic-census: --game-root or SHIFT_GAME_ROOT needed\n");
    return 2;
  }
  traffic::NavMap map;
  traffic::TrafficConfig config;
  std::string error;
  if (!traffic::read_nav(game / "media" / "tracks" / "colorado" / "colorado.nav", map, &error) ||
      !traffic::read_traffic_config(game, config, &error)) {
    std::fprintf(stderr, "traffic-census: %s\n", error.c_str());
    return 2;
  }
  traffic::Roads roads;
  roads.build(map);

  Phase noon, night, cruise;
  int samples = 0, off_road = 0, two_way = 0, right = 0, spawned = 0, away = 0, despawned = 0;
  std::vector<double> offsets, speeds, spawn_d, despawn_d;
  for (int k = 0; k < seeds; ++k) {
    const Result r = run(roads, config, seed + uint64_t(k), verbose && k == 0);
    std::printf("seed %llu: noon %.2f (%d-%d), 02:00 %.2f (%d-%d), cruise %.2f (%d-%d); %d spawns (%d away), "
                "%d despawns\n",
                (unsigned long long)(seed + uint64_t(k)), r.noon.mean(), r.noon.min, r.noon.max, r.night.mean(),
                r.night.min, r.night.max, r.cruise.mean(), r.cruise.min, r.cruise.max, r.stats.spawned,
                r.stats.spawned_away, r.stats.despawned);
    noon.sum += r.noon.mean(), noon.samples++;
    night.sum += r.night.mean(), night.samples++;
    cruise.sum += r.cruise.mean(), cruise.samples++;
    samples += r.car_samples, off_road += r.off_road, two_way += r.two_way, right += r.right;
    spawned += r.stats.spawned, away += r.stats.spawned_away, despawned += r.stats.despawned;
    offsets.insert(offsets.end(), r.offsets.begin(), r.offsets.end());
    speeds.insert(speeds.end(), r.straight_speeds.begin(), r.straight_speeds.end());
    spawn_d.insert(spawn_d.end(), r.stats.spawn_distances.begin(), r.stats.spawn_distances.end());
    despawn_d.insert(despawn_d.end(), r.stats.despawn_distances.begin(), r.stats.despawn_distances.end());
  }
  std::sort(spawn_d.begin(), spawn_d.end());
  std::sort(despawn_d.begin(), despawn_d.end());
  auto pct = [](const std::vector<double>& v, double p) { return v.empty() ? 0.0 : v[size_t(p * double(v.size() - 1))]; };
  std::printf("\nmean over %d seeds (oracle):\n", seeds);
  std::printf("  noon parked    %.2f cars (6.1)\n", noon.mean());
  std::printf("  02:00 parked   %.2f cars (0.9)\n", night.mean());
  std::printf("  noon cruising  %.2f cars (4.4)\n", cruise.mean());
  std::printf("  moving samples %d, off road (> 15 m) %d\n", samples, off_road);
  std::printf("  two-way: right of the centre line %.1f %%, median offset %.2f m\n",
              two_way ? 100.0 * right / two_way : 0.0, median(offsets));
  std::printf("  spawns %d at %.0f to %.0f m (5-95 %%), %.0f %% away (5 of 21)\n", spawned, pct(spawn_d, 0.05),
              pct(spawn_d, 0.95), spawned ? 100.0 * away / spawned : 0.0);
  std::printf("  despawns %d at %.0f to %.0f m\n", despawned, pct(despawn_d, 0.05), pct(despawn_d, 0.95));
  std::printf("  straight-road speed median %.2f m/s (13.4)\n", median(speeds));

  int misses = 0;
  auto gate = [&](bool ok, const char* what) {
    if (!ok) std::printf("MISS %s\n", what), ++misses;
  };
  gate(std::fabs(noon.mean() - 6.1) <= 0.3 * 6.1, "noon count within 30 % of 6.1");
  gate(std::fabs(night.mean() - 0.9) <= 0.3 * 0.9, "02:00 count within 30 % of 0.9");
  gate(off_road == 0, "every car within 15 m of a traffic road");
  gate(two_way == 0 || right >= 0.95 * two_way, "right of the centre line in 95 % of samples");
  gate(std::fabs(median(offsets) - 2.2) <= 0.5, "offset 2.2 +- 0.5 m");
  gate(pct(spawn_d, 0.05) >= 317 - 30 && pct(spawn_d, 0.95) <= 347 + 30, "spawns within 30 m of 317-347 m");
  gate(despawn_d.empty() || (pct(despawn_d, 0.05) >= 392 - 30 && pct(despawn_d, 0.95) <= 395 + 30),
       "despawns within 30 m of 392-395 m");
  gate(std::fabs(median(speeds) - 13.41) <= 1.0, "speed within 1 m/s of 30 mph on straight road");
  std::printf(misses ? "traffic-census: %d miss(es)\n" : "traffic-census: ok\n", misses);
  return misses ? 1 : 0;
}
