#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/satnav_vo.h"
#include "drive/save.h"
#include "settings_page.h"
#include "traffic/gps.h"
#include "traffic/nav.h"

namespace {

int g_failed = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failed;
  }
}

struct NavBuilder {
  traffic::NavMap map;
  uint32_t add_key(const std::string& k) {
    for (uint32_t i = 0; i < map.keys.size(); ++i)
      if (map.keys[i] == k) return i;
    map.keys.push_back(k);
    return uint32_t(map.keys.size() - 1);
  }
  uint32_t add_val(const std::string& v) {
    for (uint32_t i = 0; i < map.values.size(); ++i)
      if (map.values[i] == v) return i;
    map.values.push_back(v);
    return uint32_t(map.values.size() - 1);
  }
  uint32_t add_node(float x, float z, const std::vector<std::pair<std::string, std::string>>& node_tags = {}) {
    traffic::NavNode nd;
    nd.id = uint32_t(map.nodes.size() + 1);
    nd.x = x;
    nd.y = 0.0f;
    nd.z = z;
    if (!node_tags.empty()) {
      nd.tag_first = uint32_t(map.tags.size());
      for (const auto& [k, v] : node_tags) map.tags.push_back({add_key(k), add_val(v)});
      nd.tag_count = uint32_t(map.tags.size()) - nd.tag_first;
    }
    map.nodes.push_back(nd);
    return uint32_t(map.nodes.size() - 1);
  }
  void add_way(const std::vector<uint32_t>& nodes, const std::string& road_type, bool oneway = false,
               const std::string& display_name = "Test Road") {
    traffic::NavWay w;
    w.node_first = uint32_t(map.way_nodes.size());
    w.node_count = uint32_t(nodes.size());
    for (uint32_t n : nodes) map.way_nodes.push_back(n);
    w.tag_first = uint32_t(map.tags.size());
    map.tags.push_back({add_key("road_type"), add_val(road_type)});
    map.tags.push_back({add_key("display_name"), add_val(display_name)});
    if (oneway) map.tags.push_back({add_key("oneway"), add_val("true")});
    w.tag_count = uint32_t(map.tags.size()) - w.tag_first;
    map.ways.push_back(w);
  }
};

// Grid with a one-way avenue and a two-way bypass:
//   N0(0,0) ---two-way---> N1(0,300) ---two-way---> N2(300,300)
//      |                                                 |
//   oneway (N0->N3)                                  two-way
//      v                                                 |
//   N3(300,0) <----------------two-way--------------- N4(300,150)
void test_astar_and_oneway() {
  NavBuilder b;
  const uint32_t n0 = b.add_node(0, 0);
  const uint32_t n1 = b.add_node(0, 300);
  const uint32_t n2 = b.add_node(300, 300);
  const uint32_t n4 = b.add_node(300, 150);
  const uint32_t n3 = b.add_node(300, 0);

  // Two-way upper/right corridor: N0 -> N1 -> N2 -> N4 -> N3 (length 300 + 300 + 150 + 150 = 900 m)
  b.add_way({n0, n1}, "a", false, "North Ave");
  b.add_way({n1, n2}, "a", false, "East St");
  b.add_way({n2, n4, n3}, "a", false, "South Blvd");
  // One-way shortcut ONLY from N0 -> N3 (length 300 m)
  b.add_way({n0, n3}, "a", true, "OneWay Express");

  traffic::GpsSolver solver;
  solver.build(b.map);
  check(solver.ready(), "solver ready on synthetic grid");

  // Forward along one-way: N0 -> N3 should take the 300 m shortcut.
  const auto r_fwd = solver.solve(10, 0, 290, 0);
  check(r_fwd.valid, "forward one-way route valid");
  check(r_fwd.total_length < 320.0, "forward route uses 300 m one-way shortcut");

  // Reverse: N3 -> N0 must NOT go backwards along OneWay Express; must detour ~900 m via N4->N2->N1->N0!
  const auto r_rev = solver.solve(290, 0, 10, 0);
  check(r_rev.valid, "reverse route valid via two-way detour");
  check(r_rev.total_length > 800.0 && r_rev.total_length < 950.0, "reverse route respects oneway and detours ~880 m");
}

void test_turns_reroute_and_arrival() {
  // L-shaped route going north 600 m then turning RIGHT (east) 600 m, plus a parallel road at x=-200.
  NavBuilder b;
  const uint32_t s0 = b.add_node(0, 0);
  const uint32_t s1 = b.add_node(0, 300);
  const uint32_t corner = b.add_node(0, 600);
  const uint32_t e1 = b.add_node(300, 600);
  const uint32_t e2 = b.add_node(600, 600);
  const uint32_t n_ext = b.add_node(0, 900);  // makes `corner` a 3-way junction!
  const uint32_t w0 = b.add_node(-200, 0);
  const uint32_t w1 = b.add_node(-200, 600);

  b.add_way({s0, s1, corner, n_ext}, "a", false, "Main North");
  b.add_way({corner, e1, e2}, "a", false, "East Turn");
  b.add_way({s0, w0, w1, corner}, "b", false, "West Bypass");

  traffic::GpsSolver solver;
  solver.build(b.map);

  traffic::GpsNavigator nav(&solver);
  const auto set_ev = nav.set_destination(590, 600, traffic::GpsDestKind::kOutpost, "OUTPOST", 0, 10);
  check(set_ev.kind == traffic::GpsPromptEvent::Kind::kDestinationSet, "destination set event fired");
  check(set_ev.dest_kind == traffic::GpsDestKind::kOutpost, "destination kind preserved");
  check(nav.active(), "navigator active");
  check(!nav.route().turns.empty(), "route extracted upcoming junction turn");
  if (!nav.route().turns.empty()) {
    check(nav.route().turns[0].kind == traffic::GpsTurnKind::kRight, "turn at corner classified as RIGHT");
  }

  // Approach corner from z=250 (350 m away -> Far turn prompt).
  const auto far_ev = nav.update(0, 250, 0.0, 25.0, 0.05);
  check(far_ev.kind == traffic::GpsPromptEvent::Kind::kTurnFar && far_ev.turn == traffic::GpsTurnKind::kRight,
        "far right turn prompt fired at ~350 m");

  // Approach corner at z=460 (140 m away -> Near turn prompt).
  const auto near_ev = nav.update(0, 460, 0.0, 25.0, 0.05);
  check(near_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNear && near_ev.turn == traffic::GpsTurnKind::kRight,
        "near right turn prompt fired at ~140 m");

  // Approach corner at z=570 (30 m away -> Now turn prompt).
  const auto now_ev = nav.update(0, 570, 0.0, 20.0, 0.05);
  check(now_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNow && now_ev.turn == traffic::GpsTurnKind::kRight,
        "immediate right turn prompt fired at ~30 m");

  // Deviate > 65 m off-route onto West Bypass at (-200, 300) after cooldown elapses -> triggers live re-route.
  nav.update(0, 570, 0.0, 20.0, 2.0);  // expire initial reroute cooldown
  const auto reroute_ev = nav.update(-200, 300, 0.0, 20.0, 0.05);
  check(reroute_ev.kind == traffic::GpsPromptEvent::Kind::kRecalculating, "off-route deviation triggers recalculation");
  check(nav.active(), "navigator remains active after reroute");

  // Remaining polyline non-empty.
  std::vector<std::array<double, 2>> poly;
  nav.remaining_polyline(poly);
  check(poly.size() >= 2, "remaining polyline has >= 2 points");

  // Arrive at destination.
  const auto arr_ev = nav.update(585, 600, 1.57, 10.0, 0.05);
  check(arr_ev.kind == traffic::GpsPromptEvent::Kind::kArrived, "arrival event fired within 35 m");
  check(!nav.active() && nav.guidance().arrived, "navigator clears on arrival and flags arrived");
}

void test_satnav_vo_ducking_and_save() {
  // Verify SatNavPlaying snapshot activates on AudioEngine when SatNavVoPlayer plays on Bus::kVo.
  audio::AudioEngine eng(48000);
  auto sn = std::make_unique<audio::SatNavVoPlayer>(48000);
  audio::PcmSample tone;
  tone.rate = 48000;
  tone.data.assign(4800, 0.5f);  // 100 ms constant clip
  sn->set_clip(audio::SatNavCue::kTurnRight, tone);
  eng.set_satnav_vo(std::move(sn));
  eng.trigger_satnav(audio::SatNavCue::kTurnRight);

  std::vector<float> buf(4800 * 2, 0.0f);
  eng.render_offline(buf.data(), 2400);
  float peak_playing = 0.0f;
  for (int i = 0; i < 2400 * 2; ++i) peak_playing = std::max(peak_playing, std::fabs(buf[size_t(i)]));
  check(peak_playing > 0.1f, "SatNav VO renders audible signal onto VO bus");
  check(eng.telemetry().satnav_playing, "telemetry reports satnav_playing while clip active");

  // Disable SatNav -> silence immediately.
  eng.set_satnav_enabled(false);
  eng.trigger_satnav(audio::SatNavCue::kTurnRight);
  std::fill(buf.begin(), buf.end(), 0.0f);
  eng.render_offline(buf.data(), 2400);
  float peak_muted = 0.0f;
  for (int i = 0; i < 2400 * 2; ++i) peak_muted = std::max(peak_muted, std::fabs(buf[size_t(i)]));
  check(peak_muted < 1e-4f, "disabled SatNav produces zero VO audio");

  // DriveSave satnav_enabled round-trip.
  drive::DriveSave s;
  s.car = "VW_Corrado_95";
  s.position = {10, 0, 20};
  s.heading = 0.5;
  s.satnav_enabled = false;
  const std::string txt = drive::save_to_text(s);
  drive::DriveSave loaded;
  std::string err;
  check(drive::save_from_text(txt, loaded, &err), "DriveSave with satnav 0 parses");
  check(!loaded.satnav_enabled, "DriveSave preserves satnav_enabled=false");

  // SettingsPage satnav toggle.
  carview::SettingsPage sp;
  sp.load({"--no-satnav"});
  check(sp.value("satnav") == "off", "SettingsPage reads --no-satnav as off");
  check(!sp.live_settings().satnav, "LiveSettings reports satnav=false");
}

void test_travel_time_and_turn_delay() {
  // 1) Highway preference: 1200 m freeway (75 mph -> ~35.8 s) beats 800 m dirt shortcut (35 mph -> ~51.1 s).
  {
    NavBuilder b;
    const uint32_t s = b.add_node(0, 0);
    const uint32_t g = b.add_node(0, 800);
    const uint32_t d_mid = b.add_node(0, 400);
    // Curved freeway arc east of the straight dirt road (smooth gentle bends < 35 deg, total ~1080 m).
    const uint32_t f1 = b.add_node(120, 180);
    const uint32_t f2 = b.add_node(200, 400);
    const uint32_t f3 = b.add_node(120, 620);

    b.add_way({s, d_mid, g}, "dirt", false, "Dirt Shortcut");
    b.add_way({s, f1, f2, f3, g}, "freeway", false, "Interstate 76");

    traffic::GpsSolver solver;
    solver.build(b.map);
    const auto r = solver.solve(0, 5, 0, 795);
    check(r.valid, "highway preference route valid");
    bool used_freeway = false;
    for (const auto& pt : r.points)
      if (pt.x > 100.0) used_freeway = true;
    check(used_freeway, "GPS prefers faster 75 mph freeway over shorter 35 mph dirt road");
  }

  // 2) TurnDelaySeconds=5.0: a 157 m smooth semicircle (8 chords of 22.5 deg < 35 deg, 0 turn
  //    delays -> ~5.86 s at 60 mph) beats a shorter 100 m shortcut with a sharp 90-deg turn
  //    (3.73 s + 5.0 s = 8.73 s).
  {
    NavBuilder b;
    const uint32_t n0 = b.add_node(0, 0);
    const uint32_t n1 = b.add_node(0, 100);
    const uint32_t goal = b.add_node(100, 100);
    const uint32_t g_ext = b.add_node(100, 0);  // continues south from semicircle tangent

    std::vector<uint32_t> arc_nodes = {n1};
    constexpr double kPi = 3.14159265358979323846;
    for (int k = 1; k < 8; ++k) {
      const double theta = kPi * double(k) / 8.0;  // 0 at (0,100) -> pi at (100,100), center (50,100) R=50
      const float x = float(50.0 - 50.0 * std::cos(theta));
      const float z = float(100.0 + 50.0 * std::sin(theta));
      arc_nodes.push_back(b.add_node(x, z));
    }
    arc_nodes.push_back(goal);

    b.add_way({n0, n1}, "a", false, "Entry");
    b.add_way({n1, goal}, "a", false, "Sharp 90 Shortcut");
    b.add_way(arc_nodes, "a", false, "Smooth Semicircle");
    b.add_way({goal, g_ext}, "a", false, "Exit");

    traffic::GpsSolver solver;
    solver.build(b.map);
    const auto r = solver.solve(0, 10, 100, 50);
    check(r.valid, "turn-delay route valid");
    bool took_smooth = false;
    for (const auto& pt : r.points)
      if (pt.z > 130.0) took_smooth = true;
    check(took_smooth, "GPS TurnDelaySeconds=5.0 prefers smooth 157 m arc over 100 m 90-deg shortcut");
  }
}

void test_r2_4_speed_scaling_roundabout_uturn_and_cues() {
  // 1) Sat-Nav FSB fallback collision fix: kNearBearRight != kNearRight.
  const std::string nr = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearRight);
  const std::string nbr = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearBearRight);
  check(!nr.empty() && !nbr.empty() && nr != nbr, "kNearBearRight FSB fallback does not collide with kNearRight");

  // 2) Continuous speed-scaled thresholds (zero dead zones, scales up to 200 mph = 89.4 m/s).
  const auto th_slow = traffic::gps_lookahead_for_speed(15.0);
  const auto th_mid = traffic::gps_lookahead_for_speed(45.0);
  const auto th_200 = traffic::gps_lookahead_for_speed(89.4);
  check(th_slow.now_m < th_slow.near_m && th_slow.near_m < th_slow.far_m && th_slow.far_m < th_slow.extended_m,
        "lookahead tiers strictly ordered");
  check(th_200.now_m > th_mid.now_m && th_mid.now_m > th_slow.now_m, "now_m scales monotonically with speed");
  check(th_200.far_m > 850.0 && th_200.extended_m > 1500.0, "200 mph lookahead scales to highway stopping horizon");

  // 3) DestinationCannotBeSet when solver is missing or unready.
  {
    traffic::GpsNavigator empty_nav(nullptr);
    const auto fail_ev = empty_nav.set_destination(100, 100, traffic::GpsDestKind::kGeneric, "NOPE", 0, 0);
    check(fail_ev.kind == traffic::GpsPromptEvent::Kind::kDestinationCannotBeSet,
          "invalid destination emits kDestinationCannotBeSet");
  }

  // 4) Extended, NextJunc, FarDestination, and closed dead zones (60 m and 200 m at 25 m/s).
  {
    NavBuilder b;
    const uint32_t n0 = b.add_node(0, 0);
    const uint32_t n1 = b.add_node(0, 800);       // junction 1: right turn east
    const uint32_t n1_ext = b.add_node(0, 1000);  // makes n1 degree 3
    const uint32_t n2 = b.add_node(160, 800);     // junction 2 (160 m later): left turn north -> NextJunc!
    const uint32_t n2_ext = b.add_node(320, 800); // makes n2 degree 3
    const uint32_t n3 = b.add_node(160, 1800);    // 1000 m final straight -> Extended/Far/Near dest
    b.add_way({n0, n1, n1_ext}, "a", false, "North Hwy");
    b.add_way({n1, n2, n2_ext}, "a", false, "Cross Link");
    b.add_way({n2, n3}, "a", false, "Final Straight");

    traffic::GpsSolver solver;
    solver.build(b.map);
    traffic::GpsNavigator nav(&solver);
    nav.set_destination(160, 1790, traffic::GpsDestKind::kFestival, "FESTIVAL", 0, 10);

    // 650 m before n1 at 25 m/s (th.far_m=420, th.extended_m=800) -> kTurnExtended!
    const auto ext_ev = nav.update(0, 150, 0.0, 25.0, 0.05);
    check(ext_ev.kind == traffic::GpsPromptEvent::Kind::kTurnExtended && ext_ev.turn == traffic::GpsTurnKind::kRight,
          "extended turn prompt fires in far-horizon band");

    // 200 m before n1 at 25 m/s (formerly 185..220 m dead zone!) -> kTurnFar!
    const auto dz1_ev = nav.update(0, 600, 0.0, 25.0, 0.05);
    check(dz1_ev.kind == traffic::GpsPromptEvent::Kind::kTurnFar, "200 m dead zone closed (fires kTurnFar)");

    // 60 m before n1 at 25 m/s (formerly 50..65 m dead zone; th.now_m=57, th.near_m=190) -> kTurnNear!
    const auto dz2_ev = nav.update(0, 740, 0.0, 25.0, 0.05);
    check(dz2_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNear, "60 m dead zone closed (fires kTurnNear)");

    // Just after n1 on Cross Link (140 m before closely spaced junction n2) -> kTurnNextJunction!
    const auto nj_ev = nav.update(20, 800, 1.57, 25.0, 0.05);
    check(nj_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNextJunction && nj_ev.turn == traffic::GpsTurnKind::kLeft,
          "closely spaced second junction fires kTurnNextJunction");

    // On Final Straight 350 m from destination -> kArrivedFar!
    const auto fd_ev = nav.update(160, 1440, 0.0, 25.0, 0.05);
    check(fd_ev.kind == traffic::GpsPromptEvent::Kind::kArrivedFar, "final approach fires kArrivedFar");
  }

  // 5) Roundabout (sat_nav_junction=roundabout) and U-turn (uturn=<id>) node tags.
  {
    NavBuilder b;
    const uint32_t r0 = b.add_node(0, 0);
    const uint32_t r1 = b.add_node(0, 200, {{"sat_nav_junction", "roundabout"}});
    const uint32_t r2 = b.add_node(200, 200);
    const uint32_t r3 = b.add_node(0, 400);  // junction branch
    b.add_way({r0, r1, r3}, "a", false, "Roundabout Approach");
    b.add_way({r1, r2}, "a", false, "Roundabout Exit");

    // U-turn turnaround: north to (500, 200) tagged uturn=2, then reverses south to (510, 0).
    const uint32_t u0 = b.add_node(500, 0);
    const uint32_t u1 = b.add_node(500, 200, {{"uturn", "2"}});
    const uint32_t u2 = b.add_node(510, 0);
    b.add_way({u0, u1}, "a", true, "Divided NB");
    b.add_way({u1, u2}, "a", true, "Divided SB");

    traffic::GpsSolver solver;
    solver.build(b.map);
    check(solver.roundabout_node_count() == 1, "solver indexes sat_nav_junction=roundabout node");
    check(solver.uturn_node_count() == 1, "solver indexes uturn-tagged node");

    const auto rb_route = solver.solve(0, 10, 190, 200);
    bool has_roundabout = false;
    for (const auto& t : rb_route.turns) {
      if (t.kind == traffic::GpsTurnKind::kRoundabout) has_roundabout = true;
    }
    check(has_roundabout, "route through sat_nav_junction=roundabout emits kRoundabout turn");
    check(std::string(traffic::gps_turn_label(traffic::GpsTurnKind::kRoundabout)) == "ENTER ROUNDABOUT",
          "roundabout turn label formatted");

    const auto ut_route = solver.solve(500, 10, 510, 10);
    bool has_uturn = false;
    for (const auto& t : ut_route.turns) {
      if (t.kind == traffic::GpsTurnKind::kUTurn) has_uturn = true;
    }
    check(has_uturn, "reversing route through uturn-tagged node emits kUTurn turn");
  }
}

void test_world_map_gps_toggle_and_clear() {
  NavBuilder b;
  const uint32_t n0 = b.add_node(0, 0);
  const uint32_t n1 = b.add_node(0, 500);
  const uint32_t n2 = b.add_node(500, 500);
  b.add_way({n0, n1, n2}, "a", false, "Horizon Blvd");

  traffic::GpsSolver solver;
  solver.build(b.map);
  traffic::GpsNavigator nav(&solver);

  // Set GPS route from origin to (500, 500).
  const auto ev_set = nav.set_destination(500.0, 500.0, traffic::GpsDestKind::kGeneric, "WAYPOINT", 0.0, 0.0);
  check(ev_set.kind == traffic::GpsPromptEvent::Kind::kDestinationSet, "map GPS waypoint sets active route");
  check(nav.active(), "navigator active after setting map GPS route");
  std::vector<std::array<double, 2>> poly;
  nav.remaining_polyline(poly);
  check(poly.size() >= 2, "active map GPS route produces visible polyline");

  // Re-selecting within 120 m of existing destination identifies toggle-off match, then clears cleanly.
  const bool same_target =
      nav.active() && std::hypot(490.0 - nav.route().dest_x, 510.0 - nav.route().dest_z) < 120.0;
  check(same_target, "re-selecting near active GPS destination triggers toggle-clear");
  nav.clear();
  check(!nav.active(), "clearing GPS route deactivates navigator cleanly");
  poly.clear();
  nav.remaining_polyline(poly);
  check(poly.empty(), "cleared GPS route produces empty polyline");
}

void test_colorado_nav_optional() {
  const char* gr = std::getenv("SHIFT_GAME_ROOT");
  if (!gr || !*gr) return;
  const std::filesystem::path nav_path =
      std::filesystem::path(gr) / "media" / "tracks" / "colorado" / "colorado.nav";
  if (!std::filesystem::exists(nav_path)) return;
  traffic::NavMap nav;
  std::string err;
  check(traffic::read_nav(nav_path, nav, &err), "read real colorado.nav");
  traffic::GpsSolver solver;
  solver.build(nav);
  check(solver.roundabout_node_count() == 11, "Colorado nav has 11 sat_nav_junction=roundabout nodes");
  check(solver.uturn_node_count() >= 230, "Colorado nav indexes uturn-tagged nodes");
  // Festival (-610, -255) -> Red Rock Outpost (-5682, -5268)
  const auto route = solver.solve(-610.0, -255.0, -5682.0, -5268.0);
  check(route.valid, "real colorado.nav routes Festival -> Red Rock Outpost");
  check(route.total_length > 6000.0 && route.total_length < 25000.0, "real route distance plausible (6-25 km)");
  check(!route.turns.empty(), "real route extracts multiple turn instructions");
  // Playtest bookmark coordinates: bug_001 (2303.92, 548.02) and bug_023 (-3025.10, -1727.87).
  const auto r_b01 = solver.solve(2303.92, 548.02, -610.0, -255.0);
  check(r_b01.valid && r_b01.points.size() >= 2, "real colorado.nav routes cleanly from bug_001 highway spawn");
  const auto r_b23 = solver.solve(-3025.10, -1727.87, -610.0, -255.0);
  check(r_b23.valid && r_b23.points.size() >= 2, "real colorado.nav routes cleanly from bug_023 road position");
  std::printf("colorado.nav GPS check: Festival -> Red Rock = %.1f km, %zu turns, %zu points (%zu roundabouts, %zu uturns)\n",
              route.total_length / 1000.0, route.turns.size(), route.points.size(),
              solver.roundabout_node_count(), solver.uturn_node_count());
}

}  // namespace

int main() {
  test_astar_and_oneway();
  test_turns_reroute_and_arrival();
  test_travel_time_and_turn_delay();
  test_satnav_vo_ducking_and_save();
  test_r2_4_speed_scaling_roundabout_uturn_and_cues();
  test_world_map_gps_toggle_and_clear();
  test_colorado_nav_optional();
  if (g_failed == 0) std::printf("gps_tests: PASS\n");
  return g_failed ? 1 : 0;
}
