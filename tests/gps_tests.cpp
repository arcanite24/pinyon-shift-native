#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/satnav_vo.h"
#include "drive/save.h"
#define CARVIEW_GAME_HUD_LOGIC_ONLY
#include "game_hud.h"
#define CARVIEW_MAP_VIEW_LOGIC_ONLY
#include "map_view.h"
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

  // Approach corner at z=460 (140 m away, >3.0 s later -> Near turn prompt).
  const auto near_ev = nav.update(0, 460, 0.0, 25.0, 3.5);
  check(near_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNear && near_ev.turn == traffic::GpsTurnKind::kRight,
        "near right turn prompt fired at ~140 m");

  // Approach corner at z=570 (30 m away -> urgent Now turn prompt bypasses cadence).
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
  check(th_200.near_m <= 250.0 && th_200.far_m >= 380.0 && th_200.far_m <= 470.0 && th_200.extended_m <= 930.0,
        "200 mph distance-named thresholds stay anchored near true spoken distances");

  // 3) DestinationCannotBeSet when solver is missing or unready.
  {
    traffic::GpsNavigator empty_nav(nullptr);
    const auto fail_ev = empty_nav.set_destination(100, 100, traffic::GpsDestKind::kGeneric, "NOPE", 0, 0);
    check(fail_ev.kind == traffic::GpsPromptEvent::Kind::kDestinationCannotBeSet,
          "invalid destination emits kDestinationCannotBeSet");
  }

  // 4) Extended, NextJunc, FarDestination, and closed dead zones (200 m and 120 m at 25 m/s).
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

    // 650 m before n1 at 25 m/s (th.far_m=395, th.extended_m=805) -> kTurnExtended!
    const auto ext_ev = nav.update(0, 150, 0.0, 25.0, 0.05);
    check(ext_ev.kind == traffic::GpsPromptEvent::Kind::kTurnExtended && ext_ev.turn == traffic::GpsTurnKind::kRight,
          "extended turn prompt fires in far-horizon band");

    // 200 m before n1 at 25 m/s (formerly 185..220 m dead zone; >3.0 s later) -> kTurnFar!
    const auto dz1_ev = nav.update(0, 600, 0.0, 25.0, 3.5);
    check(dz1_ev.kind == traffic::GpsPromptEvent::Kind::kTurnFar, "200 m dead zone closed (fires kTurnFar)");

    // 120 m before n1 at 25 m/s (closing 4.8 s >= 3.5 s, >3.0 s later) -> kTurnNear!
    const auto dz2_ev = nav.update(0, 680, 0.0, 25.0, 3.5);
    check(dz2_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNear, "120 m near-band prompt fires kTurnNear");

    // Just after n1 on Cross Link (140 m before closely spaced junction n2, >3.0 s later) -> kTurnNextJunction!
    const auto nj_ev = nav.update(20, 800, 1.57, 25.0, 3.5);
    check(nj_ev.kind == traffic::GpsPromptEvent::Kind::kTurnNextJunction && nj_ev.turn == traffic::GpsTurnKind::kLeft,
          "closely spaced second junction fires kTurnNextJunction");

    // On Final Straight 350 m from destination (>3.0 s later) -> kArrivedFar!
    const auto fd_ev = nav.update(160, 1440, 0.0, 25.0, 3.5);
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

void test_r3_aud_11_roundabout_disambiguation_fade_and_cadence() {
  // 1) SatNav roundabout exit / keep-left / keep-right cues & FSB fallback disambiguation.
  {
    const std::string nd = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearDestination);
    const std::string fd = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kFarDestination);
    const std::string ed = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kExtendedDestination);
    check(fd.find(nd) == std::string::npos && ed.find(nd) == std::string::npos,
          "kNearDestination FSB needle does not falsely match Far/Extended destination");

    const std::string nr = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearRight);
    const std::string nbr = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearBearRight);
    const std::string nsr = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearSharpRight);
    check(nbr.find(nr) == std::string::npos && nsr.find(nr) == std::string::npos,
          "kNearRight FSB needle does not falsely match NearBear/NearSharp");

    const std::string nsl = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kNearSharpLeft);
    const std::string sl = audio::satnav_cue_fsb_fallback(audio::SatNavCue::kSharpLeft);
    check(nsl != sl && nsl.find(sl) == std::string::npos,
          "kNearSharpLeft FSB needle is disambiguated from kSharpLeft");

    std::vector<std::string> seen;
    for (int i = 0; i < int(audio::SatNavCue::kCount); ++i) {
      const std::string n = audio::satnav_cue_fsb_fallback(audio::SatNavCue(i));
      check(!n.empty(), "every SatNavCue including roundabout/keep cues has non-empty FSB needle");
      for (const auto& prev : seen) {
        if (prev == n) check(false, "every SatNavCue FSB fallback needle is unique");
      }
      seen.push_back(n);
    }
    check(std::string(audio::satnav_cue_fsb_fallback(audio::SatNavCue::kRoundaboutExit1)) == "Roundabout_Exit1",
          "kRoundaboutExit1 mapped");
    check(std::string(audio::satnav_cue_fsb_fallback(audio::SatNavCue::kRoundaboutExit4)) == "Roundabout_Exit4",
          "kRoundaboutExit4 mapped");
    check(std::string(audio::satnav_cue_fsb_fallback(audio::SatNavCue::kKeepLeft)) == "Keep_Left", "kKeepLeft mapped");
    check(std::string(audio::satnav_cue_fsb_fallback(audio::SatNavCue::kKeepRight)) == "Keep_Right",
          "kKeepRight mapped");
  }

  // 2) SatNavVoPlayer equal-power mono pan (0.70710678f) and 8 ms smooth interrupt crossfade.
  {
    audio::SatNavVoPlayer sn(48000);
    audio::PcmSample a, b;
    a.rate = b.rate = 48000;
    a.data.assign(4800, 0.6f);
    b.data.assign(4800, -0.6f);
    sn.set_clip(audio::SatNavCue::kRoundaboutExit2, a);
    sn.set_clip(audio::SatNavCue::kRecalculating, b);

    sn.trigger(audio::SatNavCue::kRoundaboutExit2);
    std::vector<float> buf(480 * 2, 0.0f);
    check(sn.render(buf.data(), 480), "SatNav renders roundabout exit cue");
    const float expected_pan = 0.6f * 0.70710678f;
    check(std::fabs(buf[10] - expected_pan) < 1e-4f && std::fabs(buf[11] - expected_pan) < 1e-4f,
          "SatNav mono pan normalized to equal-power 0.70710678f");

    // Interrupt mid-clip with opposite polarity clip -> 8 ms crossfade eliminates click.
    const float pre_int = buf[(480 - 1) * 2];
    sn.trigger(audio::SatNavCue::kRecalculating);
    std::vector<float> int_buf(480 * 2, 0.0f);
    check(sn.render(int_buf.data(), 480), "SatNav renders interrupting cue smoothly");
    float max_step = std::fabs(int_buf[0] - pre_int);
    for (int i = 1; i < 480; ++i) {
      max_step = std::max(max_step, std::fabs(int_buf[size_t(i) * 2] - int_buf[size_t(i - 1) * 2]));
    }
    check(max_step < 0.01f, "SatNav 8 ms interrupt crossfade eliminates step click");
    check(std::fabs(int_buf[450 * 2] - (-expected_pan)) < 1e-3f,
          "SatNav reaches interrupting clip level after 8 ms (384 frames)");
  }

  // 3) GpsSolver roundabout_exit numbering (1=right, 2=straight, 3=left, 4=u-turn) & event propagation.
  {
    NavBuilder b;
    const uint32_t south = b.add_node(0, 0);
    const uint32_t rb = b.add_node(0, 300, {{"sat_nav_junction", "roundabout"}});
    const uint32_t east = b.add_node(300, 300);
    const uint32_t north = b.add_node(0, 600);
    const uint32_t west = b.add_node(-300, 300);
    const uint32_t south_ret = b.add_node(15, 0);  // return leg for 4th exit U-turn
    b.add_way({south, rb}, "a", true, "South Entry");
    b.add_way({rb, east}, "a", false, "East Exit");
    b.add_way({rb, north}, "a", false, "North Exit");
    b.add_way({rb, west}, "a", false, "West Exit");
    b.add_way({rb, south_ret}, "a", true, "South Return");

    traffic::GpsSolver solver;
    solver.build(b.map);

    auto find_rb_exit = [](const traffic::GpsRoute& r) {
      for (const auto& t : r.turns) {
        if (t.kind == traffic::GpsTurnKind::kRoundabout) return t.roundabout_exit;
      }
      return 0;
    };
    check(find_rb_exit(solver.solve(0, 10, 290, 300)) == 1, "roundabout right turn selects exit 1");
    check(find_rb_exit(solver.solve(0, 10, 0, 590)) == 2, "roundabout straight selects exit 2");
    check(find_rb_exit(solver.solve(0, 10, -290, 300)) == 3, "roundabout left turn selects exit 3");
    check(find_rb_exit(solver.solve(0, 10, 15, 10)) == 4, "roundabout full loop selects exit 4");

    traffic::GpsNavigator nav(&solver);
    nav.set_destination(-290, 300, traffic::GpsDestKind::kGeneric, "WEST", 0, 10);
    const auto ev = nav.update(0, 270, 0.0, 20.0, 0.05);
    check(ev.kind == traffic::GpsPromptEvent::Kind::kTurnNow && ev.turn == traffic::GpsTurnKind::kRoundabout &&
              ev.roundabout_exit == 3,
          "GpsNavigator propagates roundabout_exit=3 on GpsPromptEvent");
  }

  // 4) 3.0 s minimum cadence between non-urgent prompts & < 3.5 s high-speed intermediate skip.
  {
    NavBuilder b;
    const uint32_t n0 = b.add_node(0, 0);
    const uint32_t corner = b.add_node(0, 1000);
    const uint32_t n_ext = b.add_node(0, 1200);
    const uint32_t east = b.add_node(600, 1000);
    b.add_way({n0, corner, n_ext}, "a", false, "Highway");
    b.add_way({corner, east}, "a", false, "Exit Ramp");

    traffic::GpsSolver solver;
    solver.build(b.map);
    traffic::GpsNavigator nav(&solver);
    nav.set_destination(590, 1000, traffic::GpsDestKind::kGeneric, "RAMP", 0, 10);

    // 1) Fire Extended at d=700 m -> starts 3.0 s prompt cooldown.
    const auto e1 = nav.update(0, 300, 0.0, 25.0, 0.05);
    check(e1.kind == traffic::GpsPromptEvent::Kind::kTurnExtended, "initial Extended prompt fires");

    // 2) Within 1.0 s (< 3.0 s cadence), Far at d=350 m must be suppressed!
    const auto e2 = nav.update(0, 650, 0.0, 25.0, 1.0);
    check(e2.kind == traffic::GpsPromptEvent::Kind::kNone,
          "non-urgent Far prompt suppressed inside 3.0 s cadence window");

    // 3) After >= 3.0 s total elapsed, Near at d=150 m (closing 6.0 s >= 3.5 s) fires cleanly.
    const auto e3 = nav.update(0, 850, 0.0, 25.0, 2.5);
    check(e3.kind == traffic::GpsPromptEvent::Kind::kTurnNear,
          "non-urgent Near prompt fires once 3.0 s cadence elapsed");

    // 4) High-speed 200 mph (89.4 m/s) approach: Near at d=200 m has closing time 2.24 s < 3.5 s -> skipped!
    nav.set_destination(590, 1000, traffic::GpsDestKind::kGeneric, "RAMP", 0, 10);
    const auto hs_near = nav.update(0, 800, 0.0, 89.4, 4.0);
    check(hs_near.kind == traffic::GpsPromptEvent::Kind::kNone,
          "200 mph intermediate Near prompt skipped when closing time < 3.5 s");

    // 5) Urgent TurnNow ALWAYS fires immediately even with 0.05 s since last prompt!
    const auto hs_now = nav.update(0, 880, 0.0, 89.4, 0.05);
    check(hs_now.kind == traffic::GpsPromptEvent::Kind::kTurnNow,
          "urgent TurnNow always bypasses cadence and closing-time skip");
  }
}

void test_r3_ui_12_minimap_compass_legend_and_filter_tabs() {
  constexpr double kPi = 3.14159265358979323846;

  // 1) Rotating "N" compass rim marker positions at cardinal headings on a 200x200 minimap.
  {
    const auto north_up = carview::minimap_compass_north_pos(0.0, 200);
    check(std::fabs(north_up[0] - 100.0) < 1e-4 && north_up[1] < 25.0,
          "north-up compass badge sits dead-top of minimap rim");

    // Facing East (+pi/2): North rotates 90 deg counter-clockwise to the left rim.
    const auto east = carview::minimap_compass_north_pos(0.5 * kPi, 200);
    check(east[0] < 25.0 && std::fabs(east[1] - 100.0) < 1e-4,
          "facing East puts North compass badge on left rim");

    // Facing South (pi): North sits on bottom rim.
    const auto south = carview::minimap_compass_north_pos(kPi, 200);
    check(std::fabs(south[0] - 100.0) < 1e-4 && south[1] > 175.0,
          "facing South puts North compass badge on bottom rim");

    // Facing West (-pi/2): North sits on right rim.
    const auto west = carview::minimap_compass_north_pos(-0.5 * kPi, 200);
    check(west[0] > 175.0 && std::fabs(west[1] - 100.0) < 1e-4,
          "facing West puts North compass badge on right rim");
  }

  // 2) World Map legend de-duplication: zero duplicate labels and no redundant "DRIVEN ROAD" entry.
  {
    const auto entries = carview::map_legend_entries();
    check(entries.size() >= 6, "map legend has expected distinct icon entries");
    std::set<std::string> seen;
    bool has_driven_road = false;
    bool has_dup = false;
    for (const auto& e : entries) {
      const std::string lbl = e.label ? std::string(e.label) : std::string();
      if (lbl == "DRIVEN ROAD") has_driven_road = true;
      if (!seen.insert(lbl).second) has_dup = true;
    }
    check(!has_driven_road, "map legend removes duplicate DRIVEN ROAD entry");
    check(!has_dup, "map legend entries are strictly unique");
  }

  // 3) Clickable top filter tab strip layout: 7 non-overlapping hit rects inside viewport bounds.
  {
    const std::vector<std::string> tabs = {"ALL",      "EVENTS",  "FESTIVAL", "OUTPOSTS",
                                           "PR STUNTS", "SPEED",  "DISCOVERY"};
    const auto rects = carview::layout_map_filter_tabs(tabs, 64.0f, 34.0f, 22.0f, 1.0f, 920.0f);
    check(rects.size() == tabs.size(), "all 7 map filter tabs lay out cleanly within header bar");
    for (size_t i = 0; i < rects.size(); ++i) {
      check(rects[i].filter_index == int(i), "map filter tab rect preserves filter index");
      check(rects[i].x1 > rects[i].x0 && rects[i].y1 > rects[i].y0, "map filter tab rect has positive area");
      check(rects[i].x0 >= 64.0f && rects[i].x1 <= 920.0f, "map filter tab rect stays inside header bounds");
      if (i > 0) {
        check(rects[i].x0 > rects[i - 1].x1, "adjacent map filter tabs do not overlap");
      }
    }
  }
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
  test_r3_aud_11_roundabout_disambiguation_fade_and_cadence();
  test_r3_ui_12_minimap_compass_legend_and_filter_tabs();
  if (g_failed == 0) std::printf("gps_tests: PASS\n");
  return g_failed ? 1 : 0;
}
