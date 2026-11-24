// SPEC-TRAFFIC over the player's own files (no game data in the
// repository): colorado.nav's layout and drivable network, the freeroam
// settings, the time-of-day factor, and the density check at the oracle's
// parked spot. Skipped (exit 77) unless SHIFT_GAME_ROOT names the disc.
// Spec: SPEC-TRAFFIC (The road network, Tags, Time-of-day factor, Density,
// Tests).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include "drive/vehicle.h"
#include "traffic/bodies.h"
#include "traffic/config.h"
#include "traffic/nav.h"
#include "traffic/population.h"
#include "traffic/roads.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void test_time_factor() {
  traffic::TrafficConfig c;  // the disc's times are the defaults
  CHECK(near(traffic::time_factor(12, c), 1, 1e-12));
  CHECK(near(traffic::time_factor(2, c), 0, 1e-12));
  CHECK(near(traffic::time_factor(6.5, c), 0.5, 1e-12));   // halfway through the morning ramp (4 to 9)
  CHECK(near(traffic::time_factor(20, c), 0.5, 1e-12));    // halfway down the evening (17 to 23)
  CHECK(near(traffic::time_factor(23.5, c), 0, 1e-12));
  CHECK(near(traffic::time_factor(9.5, c), 1, 1e-12));
  CHECK(near(traffic::traffic_cap(c, 1), 16, 1e-12));
  CHECK(near(traffic::traffic_cap(c, 0), 8, 1e-12));
}

// A straight synthetic road along +z (11 nodes, 100 m apart) tagged as a
// density-5 A road.
traffic::NavMap straight_road() {
  traffic::NavMap m;
  m.keys = {"road_type", "traffic_density"};
  m.values = {"a", "5"};
  for (uint32_t i = 0; i < 11; ++i) {
    traffic::NavNode n;
    n.id = i;
    n.z = float(i * 100);
    m.nodes.push_back(n);
    m.way_nodes.push_back(i);
  }
  m.tags = {{0, 0}, {1, 1}};
  traffic::NavWay w;
  w.node_count = 11;
  w.tag_count = 2;
  m.ways.push_back(w);
  return m;
}

// The player's car runs into the back of a traffic car at 25 m/s on flat
// ground: the traffic car takes the full step within 30 m, is pushed
// (faster than its 30 mph afterwards) and the player's car slows.
void test_push() {
  const traffic::NavMap map = straight_road();
  traffic::Roads roads;
  roads.build(map);
  traffic::TrafficConfig c;
  c.initial_cars = 0;
  traffic::DensityEntry e;
  e.id = 5, e.speed_mph = 30;  // min = max = 0: no spawns
  c.traffic[5] = e;
  c.models = {{282, 0, false, false}};
  traffic::Population pop(roads, c, 7);
  traffic::Car car;
  car.model = 282;
  car.base_speed = car.speed = car.target = 30 * 0.44704;
  pop.add(car, 0, 120, 1);
  drive::CarParams params;  // a synthetic car (world_tests' sizes)
  const double r = 0.31;
  params.wheel_centres = {drive::Vec3{-0.72, r, 1.15}, drive::Vec3{0.72, r, 1.15}, drive::Vec3{-0.72, r, -1.15},
                          drive::Vec3{0.72, r, -1.15}};
  params.tyre_radius = {r, r, r, r};
  params.centre_of_mass = {0, 0.5, 0.05};
  params.size = {1.64, 1.3, 3.9};
  traffic::Bodies bodies(pop, [&](int64_t) { return &params; });
  drive::Vehicle me(params);
  me.launch({2.2, 0, 40}, 0, 25);
  const drive::FlatGround flat;
  bool was_body = false;
  double before = 0, me_before = 0;
  for (int i = 0; i < int(8 / drive::Vehicle::kStep); ++i) {
    drive::Input in;
    in.throttle = bodies.player_contacts() ? 0 : 0.6;
    me.step(in, flat);
    const auto p = me.position(), v = me.velocity();
    pop.step(drive::Vehicle::kStep, {p.x, p.y, p.z}, {v.x, v.y, v.z});
    me_before = me.speed();
    bodies.step(flat, &me);
    if (!pop.cars().empty() && pop.cars()[0].body && !was_body) {
      was_body = true;
      before = pop.cars()[0].speed;
    }
    if (bodies.player_contacts()) break;
  }
  const double me_at_contact = me_before;
  double after = 0;
  for (int i = 0; i < int(0.5 / drive::Vehicle::kStep); ++i) {
    me.step({}, flat);
    const auto p = me.position(), v = me.velocity();
    pop.step(drive::Vehicle::kStep, {p.x, p.y, p.z}, {v.x, v.y, v.z});
    bodies.step(flat, &me);
    if (!pop.cars().empty()) after = std::max(after, pop.cars()[0].speed);
  }
  std::printf("push: body at %.1f m/s, contacts %d, the player %.1f -> %.1f m/s, the traffic car up to %.1f m/s "
              "(largest impulse %.0f N s)\n",
              before, bodies.player_contacts(), me_at_contact, me.speed(), after, bodies.largest_impulse());
  CHECK(was_body && std::fabs(before - 13.41) < 1.0);
  CHECK(bodies.player_contacts() > 0);
  CHECK(after > 16);
  CHECK(me.speed() < me_at_contact - 2);
}

// The lane hold taking over a body 1.5 m right of its lane line and 0.5
// rad off its heading (inside the off-line limits, as after a nudge or on
// rejoining the line), with the player's car parked beside the lane: the
// body goes back to its line without a burst of speed and without
// launching the player's car.
void test_hold_resume() {
  const traffic::NavMap map = straight_road();
  traffic::Roads roads;
  roads.build(map);
  traffic::TrafficConfig c;
  c.initial_cars = 0;
  traffic::DensityEntry e;
  e.id = 5, e.speed_mph = 30;
  c.traffic[5] = e;
  c.models = {{282, 0, false, false}};
  traffic::Population pop(roads, c, 7);
  traffic::Car car;
  car.model = 282;
  car.base_speed = car.speed = car.target = 30 * 0.44704;
  pop.add(car, 0, 120, 1);
  drive::CarParams params;
  const double r = 0.31;
  params.wheel_centres = {drive::Vec3{-0.72, r, 1.15}, drive::Vec3{0.72, r, 1.15}, drive::Vec3{-0.72, r, -1.15},
                          drive::Vec3{0.72, r, -1.15}};
  params.tyre_radius = {r, r, r, r};
  params.centre_of_mass = {0, 0.5, 0.05};
  params.size = {1.64, 1.3, 3.9};
  traffic::Bodies bodies(pop, [&](int64_t) { return &params; });
  drive::Vehicle me(params);
  me.reset({6.0, 0, 128}, 0);  // parked beside the lane, ahead of the body
  const drive::FlatGround flat;
  drive::Input hold;
  hold.brake = 1;
  hold.handbrake = true;
  double peak = 0, me_peak = 0, lat_end = 0, yaw_peak = 0;
  bool nudged = false;
  for (int i = 0; i < int(3 / drive::Vehicle::kStep); ++i) {
    me.step(hold, flat);
    const auto p = me.position(), v = me.velocity();
    pop.step(drive::Vehicle::kStep, {p.x, p.y, p.z}, {v.x, v.y, v.z});
    bodies.step(flat, &me);
    if (pop.cars().empty() || !pop.cars()[0].body) continue;
    drive::Vehicle* b = bodies.body(pop.cars()[0].id);
    if (!nudged) {
      nudged = true;
      const auto at = b->position();
      b->launch({at.x + 1.5, at.y, at.z}, 0.5, 13.41);
      continue;
    }
    const auto bv = b->velocity();
    peak = std::max(peak, std::hypot(bv.x, bv.z));
    yaw_peak = std::max(yaw_peak, std::fabs(b->angular_velocity().y));
    me_peak = std::max(me_peak, std::hypot(me.velocity().x, me.velocity().z));
    lat_end = b->position().x - 2.2;
  }
  std::printf("hold resume: body up to %.1f m/s and %.1f rad/s, %.2f m off its line after 3 s, the parked player up "
              "to %.2f m/s, contacts %d, off line %d\n",
              peak, yaw_peak, lat_end, me_peak, bodies.player_contacts(), int(pop.cars()[0].off_line));
  CHECK(nudged);
  CHECK(peak < 16);
  CHECK(me_peak < 1);
  CHECK(std::fabs(lat_end) < 0.3);
}

// Synthetic multi-lane one-way freeway along +z (11 nodes, 100 m apart).
traffic::NavMap oneway_freeway() {
  traffic::NavMap m;
  m.keys = {"road_type", "traffic_density", "oneway"};
  m.values = {"freeway", "4", "true"};
  for (uint32_t i = 0; i < 11; ++i) {
    traffic::NavNode n;
    n.id = i;
    n.z = float(i * 100);
    m.nodes.push_back(n);
    m.way_nodes.push_back(i);
  }
  m.tags = {{0, 0}, {1, 1}, {2, 2}};
  traffic::NavWay w;
  w.node_count = 11;
  w.tag_count = 3;
  m.ways.push_back(w);
  return m;
}

// Synthetic T-junction: main road along +z (z = 0..200) meeting cross road along +x at (0, 0, 100).
traffic::NavMap t_junction() {
  traffic::NavMap m;
  m.keys = {"road_type", "traffic_density"};
  m.values = {"a", "5"};
  // Nodes: 0=(0,0,0), 1=(0,0,100), 2=(0,0,200), 3=(100,0,100), 4=(-100,0,100)
  const float pts[5][2] = {{0, 0}, {0, 100}, {0, 200}, {100, 100}, {-100, 100}};
  for (uint32_t i = 0; i < 5; ++i) {
    traffic::NavNode n;
    n.id = i;
    n.x = pts[i][0];
    n.z = pts[i][1];
    m.nodes.push_back(n);
  }
  m.way_nodes = {0, 1, 2, 4, 1, 3};
  m.tags = {{0, 0}, {1, 1}, {0, 0}, {1, 1}};
  traffic::NavWay w0;
  w0.node_first = 0, w0.node_count = 3, w0.tag_first = 0, w0.tag_count = 2;
  traffic::NavWay w1;
  w1.node_first = 3, w1.node_count = 3, w1.tag_first = 2, w1.tag_count = 2;
  m.ways = {w0, w1};
  return m;
}

void test_traffic_behaviors() {
  // 1. Multi-lane freeway lane change + indicator when blocked by slower car.
  {
    const traffic::NavMap map = oneway_freeway();
    traffic::Roads roads;
    roads.build(map);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    traffic::DensityEntry e;
    e.id = 4, e.speed_mph = 60;
    c.traffic[4] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 3);
    traffic::Car slow, fast;
    slow.model = fast.model = 282;
    slow.base_speed = slow.speed = slow.target = 10.0;
    fast.base_speed = fast.speed = fast.target = 25.0;
    pop.add(slow, 0, 140, 1);
    pop.add(fast, 0, 120, 1);
    bool changed_lane = false, indicated = false;
    for (int i = 0; i < 180; ++i) {
      pop.step(1.0 / 60.0, {50, 0, 140}, {0, 0, 0});
      if (pop.cars().size() < 2) continue;
      const auto& fc = pop.cars()[1];
      if (fc.lane_index == 1) changed_lane = true;
      if (fc.turn_left) indicated = true;
    }
    CHECK(changed_lane);
    CHECK(indicated);
    CHECK(pop.cars().size() >= 2 && pop.cars()[1].lateral < 1.0);
  }
  // 2. Player swerve avoidance & oncoming headlight flash.
  {
    const traffic::NavMap map = straight_road();
    traffic::Roads roads;
    roads.build(map);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    c.light_flash_chance = 1.0;
    traffic::DensityEntry e;
    e.id = 5, e.speed_mph = 30;
    c.traffic[5] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 5);
    traffic::Car car;
    car.model = 282;
    car.base_speed = car.speed = car.target = 13.4;
    pop.add(car, 0, 100, 1);
    bool flashed = false;
    double max_avoid = 0;
    for (int i = 0; i < 60; ++i) {
      // Oncoming player directly in traffic car's lane (+2.2 m right of centre).
      pop.step(1.0 / 60.0, {2.2, 0, 140.0 - i * 0.25}, {0, 0, -15.0});
      flashed |= pop.cars()[0].flash_headlamps;
      max_avoid = std::max(max_avoid, std::fabs(pop.cars()[0].avoid_offset));
    }
    CHECK(flashed);
    CHECK(max_avoid > 0.3);
  }
  // 3. DeadBehavior: hazard lights after HazardLightDelay, reanimate after ReanimateDelay.
  {
    const traffic::NavMap map = straight_road();
    traffic::Roads roads;
    roads.build(map);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    traffic::DensityEntry e;
    e.id = 5, e.speed_mph = 30;
    c.traffic[5] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 9);
    traffic::Car car;
    car.model = 282;
    car.base_speed = 13.4;
    car.speed = 0.0;
    car.off_line = true;
    pop.add(car, 0, 100, 1);
    pop.cars()[0].off_line = true;
    pop.cars()[0].speed = 0.0;
    bool saw_hazards = false;
    for (int i = 0; i < 240; ++i) {  // 4.0 s
      pop.step(1.0 / 60.0, {50, 0, 100}, {0, 0, 0});
      if (pop.cars().empty()) continue;
      if (pop.cars()[0].off_line && pop.cars()[0].hazard_on) saw_hazards = true;
      if (pop.cars()[0].off_line) pop.cars()[0].speed = 0.0;
    }
    CHECK(saw_hazards);
    CHECK(!pop.cars().empty() && !pop.cars()[0].off_line && !pop.cars()[0].hazard_on);
  }
  // 4. Junction give-way: approaching cross-traffic causes car to slow/yield.
  {
    const traffic::NavMap map = t_junction();
    traffic::Roads roads;
    roads.build(map);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    c.junction_force_give_way = true;
    traffic::DensityEntry e;
    e.id = 5, e.speed_mph = 30;
    c.traffic[5] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 11);
    traffic::Car car;
    car.model = 282;
    car.base_speed = car.speed = car.target = 13.4;
    pop.add(car, 0, 85, 1);  // 15 m south of junction (0,0,100)
    // Player approaching from west (-18, 0, 100) heading east at 12 m/s.
    pop.step(1.0 / 60.0, {-18, 0, 100}, {12.0, 0, 0});
    CHECK(!pop.cars().empty() && pop.cars()[0].target < 12.0);
  }
  // 5. Traffic fade-in / distance alpha ramp.
  {
    const traffic::NavMap map = straight_road();
    traffic::Roads roads;
    roads.build(map);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    c.fade_in_time = 2.0;
    c.max_full_alpha_distance = 150.0;
    c.spawn_out = 250.0;
    traffic::DensityEntry e;
    e.id = 5, e.speed_mph = 30;
    c.traffic[5] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 13);
    traffic::Car car;
    car.model = 282;
    car.base_speed = car.speed = car.target = 10.0;
    pop.add(car, 0, 100, 1);
    pop.step(0.5, {0, 0, 50}, {0, 0, 10});
    CHECK(!pop.cars().empty() && pop.cars()[0].alpha > 0.1f && pop.cars()[0].alpha < 0.6f);
    pop.step(2.0, {0, 0, 70}, {0, 0, 10});
    CHECK(!pop.cars().empty() && pop.cars()[0].alpha > 0.95f);
    // Near spawn_out distance (> max_full_alpha_distance), alpha fades down.
    pop.step(0.1, {0, 0, -100}, {0, 0, 0});
    CHECK(!pop.cars().empty() && pop.cars()[0].alpha < 0.8f);
  }
  // 6. Smooth 90-degree junction cornering & braking: bounded yaw rate, no heading snap or rail teleport.
  {
    traffic::NavMap m;
    m.keys = {"road_type", "traffic_density"};
    m.values = {"a", "5"};
    const float pts[5][2] = {{0, 0}, {0, 50}, {0, 100}, {50, 100}, {100, 100}};
    for (uint32_t i = 0; i < 5; ++i) {
      traffic::NavNode n;
      n.id = i;
      n.x = pts[i][0];
      n.z = pts[i][1];
      m.nodes.push_back(n);
    }
    m.way_nodes = {0, 1, 2, 2, 3, 4};
    m.tags = {{0, 0}, {1, 1}, {0, 0}, {1, 1}};
    traffic::NavWay w0, w1;
    w0.node_first = 0, w0.node_count = 3, w0.tag_first = 0, w0.tag_count = 2;
    w1.node_first = 3, w1.node_count = 3, w1.tag_first = 2, w1.tag_count = 2;
    m.ways = {w0, w1};
    traffic::Roads roads;
    roads.build(m);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    traffic::DensityEntry e;
    e.id = 5, e.speed_mph = 35;
    c.traffic[5] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 29);
    traffic::Car car;
    car.model = 282;
    car.base_speed = car.speed = car.target = 15.5;
    pop.add(car, 0, 20, 1);
    const double dt = 1.0 / 60.0;
    double prev_h = pop.cars()[0].heading;
    traffic::P3 prev_p = pop.cars()[0].position;
    double max_yaw_rate = 0.0, max_lat_acc = 0.0, max_step_jump = 0.0, min_speed = 1e9;
    bool rounded = false;
    for (int i = 0; i < 600; ++i) {
      // Keep player off-road so the traffic car has a clear 90-degree turn.
      pop.step(dt, {50, 0, 50}, {0, 0, 0});
      if (pop.cars().empty()) break;
      const auto& tc = pop.cars()[0];
      const double dh = std::remainder(tc.heading - prev_h, 2.0 * 3.14159265358979323846);
      const double yr = std::fabs(dh) / dt;
      max_yaw_rate = std::max(max_yaw_rate, yr);
      max_lat_acc = std::max(max_lat_acc, yr * tc.speed);
      max_step_jump = std::max(max_step_jump, std::hypot(tc.position.x - prev_p.x, tc.position.z - prev_p.z));
      min_speed = std::min(min_speed, tc.speed);
      if (tc.position.x > 25.0 && std::fabs(tc.position.z - 97.8) < 3.5) rounded = true;
      prev_h = tc.heading;
      prev_p = tc.position;
    }
    const auto end_p = pop.cars().empty() ? traffic::P3{} : pop.cars()[0].position;
    const int end_r = pop.cars().empty() ? -1 : pop.cars()[0].road;
    const double end_s = pop.cars().empty() ? -1.0 : pop.cars()[0].s;
    std::printf("traffic corner: min %.1f m/s, max yaw %.2f rad/s, max lat %.1f m/s^2, jump %.2f m, rounded %d "
                "(end %.1f,%.1f road %d s %.1f)\n",
                min_speed, max_yaw_rate, max_lat_acc, max_step_jump, int(rounded), end_p.x, end_p.z, end_r, end_s);
    CHECK(max_yaw_rate < 1.8);
    CHECK(max_lat_acc < 8.5);
    CHECK(max_step_jump < 0.8);
    CHECK(min_speed < 9.5);
    CHECK(rounded);
  }
  // 7. Parked player in traffic lane: physics traffic brakes cleanly without plowing into the player.
  {
    const traffic::NavMap map = straight_road();
    traffic::Roads roads;
    roads.build(map);
    traffic::TrafficConfig c;
    c.initial_cars = 0;
    traffic::DensityEntry e;
    e.id = 5, e.speed_mph = 35;
    c.traffic[5] = e;
    c.models = {{282, 0, false, false}};
    traffic::Population pop(roads, c, 31);
    traffic::Car car;
    car.model = 282;
    car.base_speed = car.speed = car.target = 15.5;
    pop.add(car, 0, 80, 1);
    drive::CarParams params;
    const double r = 0.31;
    params.wheel_centres = {drive::Vec3{-0.72, r, 1.15}, drive::Vec3{0.72, r, 1.15}, drive::Vec3{-0.72, r, -1.15},
                            drive::Vec3{0.72, r, -1.15}};
    params.tyre_radius = {r, r, r, r};
    params.centre_of_mass = {0, 0.5, 0.05};
    params.size = {1.64, 1.3, 3.9};
    traffic::Bodies bodies(pop, [&](int64_t) { return &params; });
    drive::Vehicle me(params);
    me.reset({2.2, 0, 135}, 0);  // stopped dead in traffic car's lane 55 m ahead
    const drive::FlatGround flat;
    drive::Input hold;
    hold.brake = 1;
    hold.handbrake = true;
    for (int i = 0; i < int(6.0 / drive::Vehicle::kStep); ++i) {
      me.step(hold, flat);
      const auto p = me.position(), v = me.velocity();
      pop.step(drive::Vehicle::kStep, {p.x, p.y, p.z}, {v.x, v.y, v.z});
      bodies.step(flat, &me);
    }
    std::printf("traffic parked avoidance: player contacts %d, traffic speed %.2f m/s\n", bodies.player_contacts(),
                pop.cars().empty() ? -1.0 : pop.cars()[0].speed);
    CHECK(bodies.player_contacts() == 0);
  }
}

}  // namespace

int main() {
  test_time_factor();
  test_push();
  test_hold_resume();
  test_traffic_behaviors();
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) {
    std::printf("traffic_tests: SHIFT_GAME_ROOT not set, corpus checks skipped\n");
    return failures ? 1 : 77;
  }
  const std::filesystem::path game(root);
  traffic::NavMap map;
  std::string error;
  if (!traffic::read_nav(game / "media" / "tracks" / "colorado" / "colorado.nav", map, &error)) {
    std::printf("FAIL %s\n", error.c_str());
    return 1;
  }
  // Header counts (the trailer and every reference are checked by the parser).
  const uint32_t want[10] = {0x0E177551, 12036, 459, 3680, 13238, 13238, 17, 312, 223, 3299};
  for (int i = 0; i < 10; ++i) CHECK(map.header[i] == want[i]);
  int back = 0;
  for (uint32_t n = 0; n < map.nodes.size(); ++n)
    for (uint32_t k = 0; k < map.nodes[n].way_ref_count; ++k) {
      const auto& r = map.node_ways[map.nodes[n].way_ref_first + k];
      back += map.way_node(r.way, r.position) == n;
    }
  CHECK(back == 13238);

  traffic::Roads roads;
  roads.build(map);
  double km = 0, km3 = 0;
  size_t segments = 0;
  std::map<std::string, double> by_type;
  struct Row {
    int ways = 0, oneway = 0;
    double km = 0;
  };
  std::map<int, Row> by_density;  // 0: none or disabled
  for (const auto& r : roads.roads()) {
    km += r.length / 1000, km3 += r.length3 / 1000;
    segments += r.points.size() - 1;
    by_type[r.type] += r.length3 / 1000;
    Row& row = by_density[r.traffic ? r.density : 0];
    row.ways++, row.oneway += r.oneway, row.km += r.length3 / 1000;
  }
  std::printf("roads %zu, %.2f km (horizontal %.2f), %zu segments\n", roads.roads().size(), km3, km, segments);
  CHECK(roads.roads().size() == 426);
  CHECK(near(km3, 238.1, 0.05));
  CHECK(segments == 11559);
  const std::map<std::string, double> types = {{"a", 90.8},     {"b", 90.0},    {"dirt", 27.2},
                                               {"freeway", 24.9}, {"none", 2.7}, {"shortcut", 2.5}};
  for (const auto& [t, k] : types) {
    std::printf("  %-8s %.2f km\n", t.c_str(), by_type[t]);
    CHECK(near(by_type[t], k, 0.051));
  }
  const std::map<int, Row> table = {{2, {106, 4, 48.8}}, {3, {54, 4, 44.5}}, {4, {47, 37, 24.9}},
                                    {5, {17, 0, 24.8}},  {6, {17, 0, 8.6}},  {7, {13, 5, 15.8}},
                                    {10, {1, 0, 0.6}},   {12, {15, 2, 5.3}}, {13, {3, 0, 7.0}},
                                    {14, {8, 0, 4.8}},   {15, {2, 0, 1.4}},  {16, {2, 0, 0.5}},
                                    {17, {20, 20, 5.7}}, {0, {121, 2, 45.4}}};
  CHECK(by_density.size() == table.size());
  for (const auto& [id, want_row] : table) {
    const Row& got = by_density[id];
    std::printf("  density %2d: %3d ways, %5.2f km, %2d one-way\n", id, got.ways, got.km, got.oneway);
    CHECK(got.ways == want_row.ways);
    CHECK(got.oneway == want_row.oneway);
    CHECK(near(got.km, want_row.km, 0.051));
  }
  // Junctions by incident road segments.
  int junctions = 0, j3 = 0, j4 = 0, j5 = 0, plain = 0, ends = 0;
  for (uint32_t n = 0; n < roads.node_count(); ++n) {
    const int d = roads.degree(n);
    junctions += d >= 3, j3 += d == 3, j4 += d == 4, j5 += d == 5, plain += d == 2, ends += d == 1;
  }
  std::printf("junctions %d (%d, %d, %d), plain %d, ends %d\n", junctions, j3, j4, j5, plain, ends);
  CHECK(junctions == 446 && j3 == 367 && j4 == 74 && j5 == 5);
  CHECK(plain == 10840 && ends == 16);

  // The handedness of the frame: on one-way freeway carriageways the
  // opposite one lies to the left of node-order travel.
  int left = 0, right = 0;
  for (int ri = 0; ri < int(roads.roads().size()); ++ri) {
    const auto& r = roads.roads()[size_t(ri)];
    if (!r.oneway || r.type != "freeway") continue;
    for (size_t k = 0; k + 1 < r.points.size(); k += 4) {
      const double mx = 0.5 * (r.points[k].x + r.points[k + 1].x), mz = 0.5 * (r.points[k].z + r.points[k + 1].z);
      double dx = r.points[k + 1].x - r.points[k].x, dz = r.points[k + 1].z - r.points[k].z;
      const double l = std::hypot(dx, dz);
      if (l <= 0) continue;
      dx /= l, dz /= l;
      const auto rt = traffic::right_of(dx, dz);
      // The other carriageway: a one-way freeway way running the other way within 60 m.
      for (double side : {-1.0, 1.0})
        for (double off = 5; off <= 60; off += 5) {
          const auto h = roads.nearest(mx + side * off * rt[0], mz + side * off * rt[1], true, 3);
          if (h.road < 0 || h.road == ri) continue;
          const auto& o = roads.roads()[size_t(h.road)];
          if (!o.oneway || o.type != "freeway") continue;
          std::array<double, 2> od;
          roads.point_at(h.road, h.s, nullptr, &od);
          if (od[0] * dx + od[1] * dz > -0.8) continue;
          (side < 0 ? left : right)++;
          off = 1e9, side = 2;
        }
    }
  }
  std::printf("freeway: opposite carriageway left %d, right %d\n", left, right);
  CHECK(left > 50 && right == 0);

  // The density check at the oracle's parked spot (SPEC-TRAFFIC Tests).
  const auto within = roads.traffic_length_within(2002.9, 960.0, 370);
  for (const auto& [id, m] : within) std::printf("  within 370 m: density %d, %.0f m\n", id, m);
  CHECK(within.size() == 2);
  CHECK(within.count(5) && near(within.at(5), 746, 5));
  CHECK(within.count(2) && near(within.at(2), 68, 5));
  const auto hit = roads.nearest(2002.9, 960.0, true);
  std::printf("parked spot: road %d (density %d), %.1f m off the centre line (%+.1f)\n", hit.road,
              hit.road >= 0 ? roads.roads()[size_t(hit.road)].density : 0, hit.distance, hit.lateral);
  CHECK(hit.road >= 0 && roads.roads()[size_t(hit.road)].density == 5);

  // The freeroam settings.
  traffic::TrafficConfig c;
  if (!traffic::read_traffic_config(game, c, &error)) {
    std::printf("FAIL %s\n", error.c_str());
    return 1;
  }
  CHECK(c.models.size() == 17);
  CHECK(c.initial_cars == 4 && c.max_loaded_models == 4);
  bool rav4 = false;
  int limited = 0;
  for (const auto& m : c.models) rav4 |= m.id == 1530, limited += m.max_active == 1;
  CHECK(!rav4 && limited == 2);
  const auto* d5 = c.density(5);
  CHECK(d5 && near(d5->min, 0.08, 1e-9) && near(d5->max, 0.8, 1e-9) && near(d5->speed_mph, 30, 1e-9));
  const auto* d6 = c.density(6);
  CHECK(d6 && near(d6->spawn_in, 240, 1e-9) && near(d6->spawn_out, 270, 1e-9) && !d6->allows(1529));
  const auto* d12 = c.density(12);
  CHECK(d12 && d12->allows(1541) && !d12->allows(282));
  CHECK(c.density(1) && c.density(1)->max == 0);
  CHECK(near(c.spawn_in, 330, 1e-9) && near(c.spawn_out, 370, 1e-9));
  CHECK(near(c.min_traffic, 8, 1e-9) && near(c.max_traffic, 16, 1e-9));
  CHECK(near(c.model_rotation_time, 30, 1e-9) && near(c.high_lod_distance, 30, 1e-9));
  CHECK(near(traffic::time_factor(12, c), 1, 1e-12) && near(traffic::time_factor(2, c), 0, 1e-12));
  // The density check's arithmetic: per 100 m within the spawn-out distance.
  double noon = 0, night = 0;
  for (const auto& [id, m] : within) {
    noon += traffic::density_at(*c.density(id), 1) * m / 100;
    night += traffic::density_at(*c.density(id), 0) * m / 100;
  }
  std::printf("local density: noon %.2f, 02:00 %.2f cars\n", noon, night);
  CHECK(near(noon, 6.2, 0.1) && near(night, 0.6, 0.05));
  CHECK(c.initial_festival_cars == 2 && c.festival_density(8) && near(c.festival_density(8)->max, 1.0, 1e-9));
  CHECK(near(c.junction_max_collision_check_distance, 60.0, 1e-9));
  CHECK(near(c.avoid_max_offset, 5.0, 1e-4));
  CHECK(near(c.dead_hazard_delay, 1.4, 1e-9) && near(c.dead_reanimate_delay, 2.0, 1e-9));

  // Race-mode traffic settings (<Settings name="race">).
  traffic::TrafficConfig rc;
  CHECK(traffic::read_traffic_config(game, rc, &error, true));
  CHECK(rc.models.size() == 6 && rc.initial_cars == 0);
  CHECK(rc.density(4) && near(rc.density(4)->speed_mph, 90.0, 1e-9));

  std::printf(failures ? "traffic_tests: %d failure(s)\n" : "traffic_tests: ok\n", failures);
  return failures ? 1 : 0;
}
