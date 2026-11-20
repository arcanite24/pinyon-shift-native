// traffic-census: SPEC-TRAFFIC's oracle census over our traffic, headless.
// The player stands at the oracle's parked spot (2002.9, 143.8, 960.0) on
// a density-5 road: 60 s at noon, then 02:00 (counted from 30 s to 60 s
// after the change, as the oracle's first night row, and settled: the
// minute after the last car of the noon population left), then 30 s
// driving along the road at 10.4 m/s (313 m). With --steady it stays
// parked at noon for 600 s instead (counted from 60 s), for the
// steady-state count and the lifetime and interval terms behind it. Each
// half second it counts the active traffic within 1 km and checks every
// car against the road map: within 15 m of a traffic road, on two-way
// roads right of the centre line at 2.2 +- 0.5 m. It also reports the
// spawn and despawn distances, the share driving away, and the speeds on
// straight road. Without --bake the population is kinematic; with --bake
// (and Colorado's collision from the disc) the cars within 30 m are bodies
// with their own rows on the collision ground (bodies.h), the player's car
// (--car, the R34 by default) among them: parked on its brakes, then
// carried along the cruise.
//
//   traffic-census --game-root <disc> [--bake <dir> [--car <media>]] [--seed N] [--seeds N] [--verbose]
//                  [--freeway] [--models] [--steady] [--at x y z]
//   traffic-census --game-root <disc> --registry <text>   (GlobalRegistry.xml lines whose id holds it)
//
// Exit 0 when every tolerance of SPEC-TRAFFIC holds over the seeds' mean,
// 1 when one misses (each miss is printed), 2 on a usage or read error.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cars/bake_format.h"
#include "cars/sources.h"
#include "drive/from_physics.h"
#include "fh1data/archive.h"
#include "traffic/bodies.h"
#include "traffic/config.h"
#include "traffic/nav.h"
#include "traffic/population.h"
#include "traffic/roads.h"
#include "world/collision.h"
#include "world/stream.h"
#include "world/world.h"

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
  Phase noon, night, settled, cruise;
  double settled_at = -1;  // s: the last noon car gone
  int parked_spawned = 0, parked_away = 0;  // spawns before the cruise, those driving away
  // Cars spawned while parked at noon: lifetimes (s) of those driving
  // towards and away from the player, and the spawn intervals (s).
  std::vector<double> life_towards, life_away, intervals;
  int car_samples = 0, off_road = 0, two_way = 0, right = 0, one_way = 0, node_order = 0;
  std::vector<double> offsets, straight_speeds;
  traffic::PopulationStats stats;
  int bodies_max = 0, contacts = 0, player_contacts = 0, body_steps = 0;
  double body_lateral_max = 0;  // the largest offset of a body from its lane line (m)
};

// The world for bodies: Colorado's collision around the player and every
// traffic model's rows.
struct Physics {
  world::World colorado;
  world::Collision collision;
  std::unique_ptr<world::Streamer> streamer;
  std::map<int64_t, drive::CarParams> params;
  drive::CarParams player;
  double torque_scale = 0.25;
};

bool ground_height(const world::Streamer& s, double x, double z, double hint, double& y) {
  drive::Hit h;
  if (!s.ground().raycast({x, hint + 4, z}, {0, -1, 0}, 12, h)) return false;
  y = hint + 4 - h.distance;
  return true;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

traffic::P3 g_spot{2002.9, 143.8, 960.0};

Result run(const traffic::Roads& roads, const traffic::TrafficConfig& config, uint64_t seed, bool verbose,
           Physics* physics, bool steady) {
  Result out;
  traffic::Population pop(roads, config, seed);
  traffic::P3 player = g_spot, velocity{0, 0, 0};
  std::unique_ptr<traffic::Bodies> bodies;
  std::unique_ptr<drive::Vehicle> me;
  if (physics) {
    const world::Streamer& s = *physics->streamer;
    pop.set_ground([&s](double x, double z, double hint, double& y) { return ground_height(s, x, z, hint, y); });
    traffic::BodiesOptions bo = traffic::bodies_options(config);
    bo.torque_scale = physics->torque_scale;
    bodies = std::make_unique<traffic::Bodies>(
        pop,
        [physics](int64_t m) -> const drive::CarParams* {
          const auto it = physics->params.find(m);
          return it == physics->params.end() ? nullptr : &it->second;
        },
        bo);
    me = std::make_unique<drive::Vehicle>(physics->player);
    double y = player.y;
    ground_height(s, player.x, player.z, player.y, y);
    // Parked facing along the road, against node order (its right-hand lane).
    const auto h = roads.nearest(player.x, player.z, true);
    std::array<double, 2> d;
    roads.point_at(h.road, h.s, nullptr, &d);
    me->reset({player.x, y, player.z}, std::atan2(-d[0], -d[1]));
    player.y = y;
  }
  // The cruise: along the player's road, the way its right-hand lane runs.
  const auto start = roads.nearest(player.x, player.z, true);
  traffic::Car walker;
  walker.road = start.road;
  walker.s = start.s;
  walker.dir = start.lateral < 0 ? -1 : 1;
  const double kDt = physics ? drive::Vehicle::kStep : 1.0 / 60, kCruise = 313.0 / 30;
  const int every = int(std::lround(0.5 / kDt));
  // The cruise starts a minute after the night settles (at 240 s if it
  // never does).
  double cruise_at = steady ? 1e9 : 240, end = steady ? 600 : cruise_at + 30;
  bool snapped = false;
  std::set<uint32_t> noon_ids, alive;
  std::map<uint32_t, std::pair<double, bool>> born;  // spawn time, driving away
  double last_spawn = -1;
  for (int i = 0;; ++i) {
    const double t = i * kDt;
    if (t > end) break;
    pop.set_hour(steady || t < 60 || t >= cruise_at ? 12 : 2);
    if (t >= cruise_at && !snapped) {
      snapped = true;
      out.parked_spawned = pop.stats().spawned, out.parked_away = pop.stats().spawned_away;
    }
    if (t >= cruise_at) {
      // Drive along the road (its centre line at the parked offset).
      walker.s += walker.dir * kCruise * kDt;
      std::array<double, 2> d;
      const auto p = roads.point_at(walker.road, walker.s, nullptr, &d);
      const auto rt = traffic::right_of(d[0] * walker.dir, d[1] * walker.dir);
      player = {p.x + rt[0] * 2.2, p.y, p.z + rt[1] * 2.2};
      velocity = {d[0] * walker.dir * kCruise, 0, d[1] * walker.dir * kCruise};
      if (me) {
        double y = player.y;
        ground_height(*physics->streamer, player.x, player.z, player.y, y);
        player.y = y;
        me->launch({player.x, player.y, player.z}, std::atan2(d[0] * walker.dir, d[1] * walker.dir), kCruise);
      }
    }
    if (me && t < cruise_at) {
      drive::Input hold;
      hold.brake = 1;
      hold.handbrake = true;
      me->step(hold, physics->streamer->ground());
      const auto at = me->position();
      player = {at.x, at.y, at.z};
      const auto v = me->velocity();
      velocity = {v.x, v.y, v.z};
    }
    if (physics) physics->streamer->update(player.x, player.z, nullptr, nullptr);
    pop.step(kDt, player, velocity);
    if (bodies) {
      bodies->step(physics->streamer->ground(), me.get());
      out.body_steps += int(bodies->count());
      out.bodies_max = std::max(out.bodies_max, int(bodies->count()));
      for (const auto& c : pop.cars())
        if (c.body && c.road >= 0) {
          const auto h = roads.nearest(c.position.x, c.position.z, true, 60);
          if (h.road >= 0) out.body_lateral_max = std::max(out.body_lateral_max, std::fabs(std::fabs(h.lateral) - 2.2));
        }
    }
    if (i % every != 0) continue;
    // Lifetimes and intervals of the cars spawned while parked at noon.
    {
      std::set<uint32_t> now;
      for (const auto& c : pop.cars()) {
        now.insert(c.id);
        if (t == 0 || alive.count(c.id)) continue;
        const bool noon_parked = steady ? t < end - 60 : t < 60;
        if (!noon_parked) continue;
        const double out_along =
            (c.position.x - player.x) * std::sin(c.heading) + (c.position.z - player.z) * std::cos(c.heading);
        born[c.id] = {t, out_along > 0};
        if (last_spawn >= 0) out.intervals.push_back(t - last_spawn);
        last_spawn = t;
      }
      for (uint32_t id : alive)
        if (!now.count(id)) {
          const auto it = born.find(id);
          if (it == born.end()) continue;
          (it->second.second ? out.life_away : out.life_towards).push_back(t - it->second.first);
          born.erase(it);
        }
      alive = now;
      if (!steady && std::fabs(t - 60) < 1e-6) noon_ids = now;
      if (!steady && t > 60 + 1e-6 && out.settled_at < 0) {
        bool left = true;
        for (uint32_t id : noon_ids) left &= !now.count(id);
        if (left) out.settled_at = t, cruise_at = t + 60, end = cruise_at + 30;
      }
    }
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
      const auto& near_road = roads.roads()[size_t(h.road)];
      if (!near_road.oneway) {
        // Its offset right of the nearest road's centre line, in the
        // direction its heading runs along that road (independent of the
        // population's own road and arc length).
        out.two_way++;
        std::array<double, 2> d;
        roads.point_at(h.road, h.s, nullptr, &d);
        const double along = std::sin(c.heading) * d[0] + std::cos(c.heading) * d[1];
        const double lat = along >= 0 ? h.lateral : -h.lateral;
        out.right += lat > 0;
        out.offsets.push_back(lat);
      } else {
        // One-way: travelling in node order.
        std::array<double, 2> d;
        roads.point_at(h.road, h.s, nullptr, &d);
        out.one_way++;
        out.node_order += std::sin(c.heading) * d[0] + std::cos(c.heading) * d[1] > 0;
      }
      // Straight road: no bend under 300 m within 60 m either way.
      bool straight = true;
      for (size_t k = 0; k < r.points.size(); ++k)
        if (std::fabs(r.s[k] - c.s) < 60 && r.radius[k] < 300) straight = false;
      if (straight && c.age > 5) out.straight_speeds.push_back(c.speed);
    }
    if (steady) {
      if (t >= 60) out.noon.add(active);
    } else {
      if (t < 60) out.noon.add(active);
      if (t >= 90 && t < 120) out.night.add(active);
      if (out.settled_at >= 0 && t >= out.settled_at && t < cruise_at) out.settled.add(active);
      if (t >= cruise_at) out.cruise.add(active);
    }
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
  if (bodies) out.contacts = bodies->contacts(), out.player_contacts = bodies->player_contacts();
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path game, bake_dir;
  std::string player_car = "NIS_SkylineGTRVSpecII_02";
  uint64_t seed = 1;
  int seeds = 10;
  bool verbose = false, check_models = false, freeway = false, steady = false;
  std::string registry_prefix;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game-root") game = next();
    else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--seeds") seeds = std::max(1, std::atoi(next().c_str()));
    else if (a == "--verbose") verbose = true;
    else if (a == "--bake") bake_dir = next();
    else if (a == "--car") player_car = next();
    else if (a == "--at") {
      g_spot.x = std::atof(next().c_str());
      g_spot.y = std::atof(next().c_str());
      g_spot.z = std::atof(next().c_str());
    } else if (a == "--models") check_models = true;
    else if (a == "--freeway") freeway = true;
    else if (a == "--registry") registry_prefix = next();
    else if (a == "--steady") steady = true;
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
  if (!registry_prefix.empty()) {
    // The GlobalRegistry.xml lines whose id contains the text.
    std::string err;
    auto z = fh1::Archive::open(game / "media" / "gametunablesettings.zip", &err);
    if (!z) return std::fprintf(stderr, "traffic-census: %s\n", err.c_str()), 2;
    const auto r = z->read("GlobalRegistry.xml");
    if (!r.ok()) return std::fprintf(stderr, "traffic-census: %s\n", r.error.c_str()), 2;
    const std::string text(r.data.begin(), r.data.end());
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find('\n', at);
      if (end == std::string::npos) end = text.size();
      const std::string line = text.substr(at, end - at);
      if (line.find(registry_prefix) != std::string::npos) std::printf("%s\n", line.c_str());
      at = end + 1;
    }
    return 0;
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
  if (freeway) {
    // The middle of the longest one-way freeway way (density 4), 12 m to
    // its left (off the carriageway).
    int best = -1;
    for (int i = 0; i < int(roads.roads().size()); ++i) {
      const auto& r = roads.roads()[size_t(i)];
      if (r.oneway && r.traffic && r.density == 4 && (best < 0 || r.length > roads.roads()[size_t(best)].length))
        best = i;
    }
    std::array<double, 2> d;
    const auto p = roads.point_at(best, roads.roads()[size_t(best)].length / 2, nullptr, &d);
    const auto rt = traffic::right_of(d[0], d[1]);
    g_spot = {p.x - rt[0] * 12, p.y, p.z - rt[1] * 12};
    std::printf("freeway spot: road %d (%.0f m), at %.1f %.1f %.1f\n", best, roads.roads()[size_t(best)].length,
                g_spot.x, g_spot.y, g_spot.z);
  }
  std::unique_ptr<Physics> physics;
  if (!bake_dir.empty()) {
    physics = std::make_unique<Physics>();
    const auto colorado = game / "media" / "tracks" / "colorado";
    std::string warning;
    if (!physics->colorado.open(colorado, std::filesystem::path("out") / "world" / "colorado.index", &error) ||
        !physics->collision.open(colorado, game / "media" / "physics.zip", &error, &warning)) {
      std::fprintf(stderr, "traffic-census: %s\n", error.c_str());
      return 2;
    }
    physics->streamer = std::make_unique<world::Streamer>(physics->colorado, world::GatherOptions{}, 200,
                                                          &physics->collision);
    if (!physics->streamer->update(g_spot.x, g_spot.z, nullptr, &error)) {
      std::fprintf(stderr, "traffic-census: %s\n", error.c_str());
      return 2;
    }
    // Each model's rows, by Data_Car.Id (the second line of its physics rows).
    auto params_of = [&](const std::string& media, drive::CarParams& out) {
      cars::bake::Bake b;
      if (!cars::bake::read_file(bake_dir / "cars" / (cars::lower(media) + ".shiftbake"), b, &error)) return false;
      out = drive::params_for_baked_car(b.car, bake_dir);
      return true;
    };
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(bake_dir / "cars", ec)) {
      const std::string name = f.path().filename().string();
      if (name.size() < 12 || name.substr(name.size() - 12) != ".physics.txt") continue;
      std::ifstream in(f.path());
      std::string line;
      while (std::getline(in, line) && line.rfind("car ", 0) != 0) {
      }
      long long id = 0;
      char media[128] = {};
      if (std::sscanf(line.c_str(), "car %lld %127s", &id, media) != 2) continue;
      bool traffic = false;
      for (const auto& m : config.models) traffic |= m.id == id;
      if (!traffic) continue;
      drive::CarParams p;
      if (!params_of(media, p)) {
        std::fprintf(stderr, "traffic-census: %s\n", error.c_str());
        return 2;
      }
      physics->params[id] = p;
    }
    if (!params_of(player_car, physics->player)) {
      std::fprintf(stderr, "traffic-census: %s\n", error.c_str());
      return 2;
    }
    {
      // Collision\TrafficTorqueScale from the player's car's settings rows.
      std::ifstream in(bake_dir / "cars" / (cars::lower(player_car) + ".physics.txt"));
      std::string line;
      while (std::getline(in, line))
        if (line.rfind("setting Collision\\TrafficTorqueScale ", 0) == 0)
          physics->torque_scale = std::atof(line.c_str() + line.rfind(' ') + 1);
    }
    std::printf("bodies: %zu of %zu traffic models baked, torque scale %.2f\n", physics->params.size(),
                config.models.size(), physics->torque_scale);
    if (check_models) {
      // Each model as a body on flat ground: its mass and box, then 8 s
      // launched at 30 mph along +z on the line x = 0, driven and held as
      // Bodies does (the throttle at AccelMaxInput, the lane hold).
      int bad = 0;
      const drive::FlatGround flat;
      for (const auto& [id, p] : physics->params) {
        drive::Vehicle v(p);
        v.set_ai_controls(true);
        v.launch({0, 0, 0}, 0, 13.41);
        double up = 1, slowest = 1e9, fastest = 0;
        const traffic::BodiesOptions bo = traffic::bodies_options(config);
        for (int i = 0; i < int(8 / drive::Vehicle::kStep); ++i) {
          drive::Input in;
          traffic::Bodies::speed_input(13.41, v.speed(), bo, in);
          const auto fw = drive::rotate(v.orientation(), {0, 0, 1});
          traffic::Bodies::hold_lane(v, 13.41, {0, 1}, v.position().x, -std::atan2(fw.x, fw.z), bo);
          v.step(in, flat);
          if (verbose && i % 60 == 0)
            std::printf("    %lld t %.1f v %.2f gear %d rpm %.0f thr %.2f brk %.2f clutch %.2f\n", (long long)id,
                        v.time(), v.speed(), v.gear(), v.rpm(), in.throttle, in.brake, v.clutch());
          up = std::min(up, drive::rotate(v.orientation(), {0, 1, 0}).y);
          if (i * drive::Vehicle::kStep > 2) slowest = std::min(slowest, v.speed()), fastest = std::max(fastest, v.speed());
        }
        const bool ok = up > 0.95 && slowest > 12.4 && fastest < 14.4;
        bad += !ok;
        std::printf("  model %4lld: mass %6.0f kg, box %.2f x %.2f x %.2f m; 2-8 s at %.2f to %.2f m/s, up %.3f%s\n",
                    (long long)id, p.mass, p.size.x, p.size.y, p.size.z, slowest, fastest, up, ok ? "" : "  MISS");
      }
      std::printf("models: %d miss(es)\n", bad);
      return bad ? 1 : 0;
    }
  }

  Phase noon, night, settled, cruise;
  int samples = 0, off_road = 0, two_way = 0, right = 0, spawned = 0, away = 0, despawned = 0, one_way = 0,
      node_order = 0, parked_spawned = 0, parked_away = 0, unsettled = 0;
  std::vector<double> offsets, speeds, spawn_d, despawn_d, life_towards, life_away, intervals, settled_at;
  for (int k = 0; k < seeds; ++k) {
    const Result r = run(roads, config, seed + uint64_t(k), verbose && k == 0, physics.get(), steady);
    if (physics)
      std::printf("  bodies: up to %d at once, %d contacts (%d with the player), off their lane line by up to %.2f m\n",
                  r.bodies_max, r.contacts, r.player_contacts, r.body_lateral_max);
    std::printf("seed %llu: noon %.2f (%d-%d), 02:00 %.2f (%d-%d), settled from %.0f s %.2f (%d-%d), "
                "cruise %.2f (%d-%d); %d spawns (%d away), %d despawns\n",
                (unsigned long long)(seed + uint64_t(k)), r.noon.mean(), r.noon.min, r.noon.max, r.night.mean(),
                r.night.min, r.night.max, r.settled_at, r.settled.mean(), r.settled.min, r.settled.max,
                r.cruise.mean(), r.cruise.min, r.cruise.max, r.stats.spawned, r.stats.spawned_away,
                r.stats.despawned);
    noon.sum += r.noon.mean(), noon.samples++;
    night.sum += r.night.mean(), night.samples++;
    if (r.settled_at >= 0) settled.sum += r.settled.mean(), settled.samples++, settled_at.push_back(r.settled_at - 60);
    else unsettled += !steady;
    cruise.sum += r.cruise.mean(), cruise.samples++;
    parked_spawned += r.parked_spawned, parked_away += r.parked_away;
    life_towards.insert(life_towards.end(), r.life_towards.begin(), r.life_towards.end());
    life_away.insert(life_away.end(), r.life_away.begin(), r.life_away.end());
    intervals.insert(intervals.end(), r.intervals.begin(), r.intervals.end());
    samples += r.car_samples, off_road += r.off_road, two_way += r.two_way, right += r.right;
    one_way += r.one_way, node_order += r.node_order;
    spawned += r.stats.spawned, away += r.stats.spawned_away, despawned += r.stats.despawned;
    offsets.insert(offsets.end(), r.offsets.begin(), r.offsets.end());
    speeds.insert(speeds.end(), r.straight_speeds.begin(), r.straight_speeds.end());
    spawn_d.insert(spawn_d.end(), r.stats.spawn_distances.begin(), r.stats.spawn_distances.end());
    despawn_d.insert(despawn_d.end(), r.stats.despawn_distances.begin(), r.stats.despawn_distances.end());
  }
  std::sort(spawn_d.begin(), spawn_d.end());
  std::sort(despawn_d.begin(), despawn_d.end());
  auto pct = [](const std::vector<double>& v, double p) { return v.empty() ? 0.0 : v[size_t(p * double(v.size() - 1))]; };
  auto mean_of = [](const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / double(v.size());
  };
  std::printf("\nmean over %d seeds (oracle):\n", seeds);
  if (steady) {
    std::printf("  noon parked, 60-600 s  %.2f cars (6.1)\n", noon.mean());
  } else {
    std::printf("  noon parked    %.2f cars (6.1)\n", noon.mean());
    std::printf("  02:00 parked   %.2f cars (0.9; 30-60 s after the change)\n", night.mean());
    std::printf("  02:00 settled  %.2f cars (0.8; the minute after the last noon car left, %.0f s after the "
                "change at the median; oracle 44 s; %d seed(s) never settled)\n",
                settled.mean(), median(settled_at), unsettled);
    std::printf("  noon cruising  %.2f cars (4.4)\n", cruise.mean());
    std::printf("  spawns while parked %d, %.0f %% away (SpawnAwayChanceMin %.2f)\n", parked_spawned,
                parked_spawned ? 100.0 * parked_away / parked_spawned : 0.0, config.away_chance_min);
  }
  const double share_away = life_towards.size() + life_away.size()
                                ? double(life_away.size()) / double(life_towards.size() + life_away.size())
                                : 0.0;
  std::printf("  parked at noon: %zu spawns, %.0f %% away; lifetime towards %.1f s (54), away %.1f s (5); "
              "interval %.1f s (7.4); lifetime / interval %.2f cars\n",
              life_towards.size() + life_away.size(), 100 * share_away, mean_of(life_towards), mean_of(life_away),
              mean_of(intervals),
              mean_of(intervals) > 0
                  ? ((1 - share_away) * mean_of(life_towards) + share_away * mean_of(life_away)) / mean_of(intervals)
                  : 0.0);
  std::printf("  moving samples %d, off road (> 15 m) %d\n", samples, off_road);
  std::printf("  two-way (nearest road, by heading): right of the centre line %.1f %%, median offset %.2f m\n",
              two_way ? 100.0 * right / two_way : 0.0, median(offsets));
  std::printf("  one-way samples %d, in node order %d\n", one_way, node_order);
  std::printf("  spawns %d at %.0f to %.0f m (5-95 %%), %.0f %% away (5 of 21)\n", spawned, pct(spawn_d, 0.05),
              pct(spawn_d, 0.95), spawned ? 100.0 * away / spawned : 0.0);
  std::printf("  despawns %d at %.0f to %.0f m\n", despawned, pct(despawn_d, 0.05), pct(despawn_d, 0.95));
  std::printf("  straight-road speed median %.2f m/s (13.4)\n", median(speeds));

  if (steady) return std::printf("traffic-census: steady run (not gated)\n"), 0;
  int misses = 0;
  auto gate = [&](bool ok, const char* what) {
    if (!ok) std::printf("MISS %s\n", what), ++misses;
  };
  gate(std::fabs(noon.mean() - 6.1) <= 0.3 * 6.1, "noon count within 30 % of 6.1");
  gate(std::fabs(night.mean() - 0.9) <= 0.3 * 0.9, "02:00 count within 30 % of 0.9");
  gate(unsettled == 0 && std::fabs(settled.mean() - 0.8) <= 0.3 * 0.8, "02:00 settled count within 30 % of 0.8");
  gate(off_road == 0, "every car within 15 m of a traffic road");
  gate(two_way == 0 || right >= 0.95 * two_way, "right of the centre line in 95 % of samples");
  gate(node_order == one_way, "one-way roads driven in node order");
  gate(std::fabs(median(offsets) - 2.2) <= 0.5, "offset 2.2 +- 0.5 m");
  gate(pct(spawn_d, 0.05) >= 317 - 30 && pct(spawn_d, 0.95) <= 347 + 30, "spawns within 30 m of 317-347 m");
  gate(despawn_d.empty() || (pct(despawn_d, 0.05) >= 392 - 30 && pct(despawn_d, 0.95) <= 395 + 30),
       "despawns within 30 m of 392-395 m");
  gate(std::fabs(median(speeds) - 13.41) <= 1.0, "speed within 1 m/s of 30 mph on straight road");
  std::printf(misses ? "traffic-census: %d miss(es)\n" : "traffic-census: ok\n", misses);
  return misses ? 1 : 0;
}
