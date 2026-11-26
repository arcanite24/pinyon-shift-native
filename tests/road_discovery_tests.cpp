#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "drive/save.h"
#include "freeroam/collectibles.h"
#include "freeroam/road_discovery.h"
#include "map_cursor.h"
#include "map_view.h"
#include "profile/drive_save.h"
#include "profile/profile.h"
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
  uint32_t add_node(float x, float z) {
    traffic::NavNode nd;
    nd.id = uint32_t(map.nodes.size() + 1);
    nd.x = x;
    nd.y = 0.0f;
    nd.z = z;
    map.nodes.push_back(nd);
    return uint32_t(map.nodes.size() - 1);
  }
  void add_way(const std::vector<uint32_t>& nodes, const std::string& rt, const std::string& name) {
    traffic::NavWay w;
    w.node_first = uint32_t(map.way_nodes.size());
    w.node_count = uint32_t(nodes.size());
    for (uint32_t n : nodes) map.way_nodes.push_back(n);
    w.tag_first = uint32_t(map.tags.size());
    map.tags.push_back({add_key("road_type"), add_val(rt)});
    map.tags.push_back({add_key("display_name"), add_val(name)});
    w.tag_count = uint32_t(map.tags.size()) - w.tag_first;
    map.ways.push_back(w);
  }
};

void test_synthetic_discovery_and_save() {
  NavBuilder b;
  const uint32_t a0 = b.add_node(0, 0), a1 = b.add_node(0, 100), a2 = b.add_node(0, 200);
  const uint32_t b0 = b.add_node(300, 0), b1 = b.add_node(300, 100);
  const uint32_t s0 = b.add_node(600, 0), s1 = b.add_node(600, 100);
  b.add_way({a0, a1, a2}, "a", "Highway 1");
  b.add_way({b0, b1}, "dirt", "Canyon Trail");
  b.add_way({s0, s1}, "shortcut", "Hidden Shortcut");  // not counted in drawn roads

  freeroam::RoadDiscovery rd;
  check(rd.build(b.map), "RoadDiscovery builds on synthetic nav");
  check(rd.total_roads() == 2, "counts only drawn roads (2, excluding shortcut)");
  check(rd.discovered_count() == 0, "starts with 0 discovered roads");

  auto ev0 = rd.update(0, 0);
  check(ev0.empty(), "visiting only start node a0 does not complete 200 m Highway 1");
  check(rd.is_node_discovered(a0) && !rd.is_node_discovered(a1),
        "proximity marking does not prematurely mark far endpoint a1 100 m away");
  check(rd.visited_node_count() == 1 && rd.visited_segment_count() == 0,
        "visited_node_count tracks single node while visited_segment_count stays 0");

  auto ev0b = rd.update(0, 100);
  check(ev0b.empty(), "100 m unvisited segment remaining (> 75 m threshold) does not complete Highway 1");
  check(rd.is_node_discovered(a0) && rd.is_node_discovered(a1) && !rd.is_node_discovered(a2),
        "a0 and a1 marked discovered while a2 remains undiscovered");
  check(rd.is_segment_discovered(a0, a1) && !rd.is_segment_discovered(a1, a2),
        "segment discovered only when both endpoints are visited");
  check(rd.visited_segment_count() == 1, "visited_segment_count increments once segment (a0,a1) completes");

  auto ev1 = rd.update(0, 200);
  check(ev1.size() == 1 && ev1[0].road_name == "Highway 1" && ev1[0].discovered_count == 1 && ev1[0].total_count == 2,
        "driving full Highway 1 discovers road 1/2");
  check(rd.is_node_discovered(a0) && rd.is_node_discovered(a1) && rd.is_node_discovered(a2),
        "all Highway 1 nodes marked discovered");
  check(!rd.is_node_discovered(b0), "other road still undiscovered");

  // Re-driving same segment emits nothing.
  auto ev1b = rd.update(0, 200);
  check(ev1b.empty(), "re-driving discovered segment emits no duplicate event");

  // Save & restore round-trip through DriveSave.
  drive::DriveSave sv;
  sv.car = "VW_Corrado_95";
  sv.position = {0, 0, 50};
  sv.heading = 0;
  sv.treasure_map = true;
  sv.road_discovery_nodes = rd.node_count();
  sv.road_discovery_words = rd.words();
  sv.discovered_outposts = {"GasStation_01", "GasStation_04"};
  sv.completed_pr_stunts = {"speedstunt_01", "prstunt_01"};

  const std::string text = drive::save_to_text(sv);
  drive::DriveSave reloaded;
  std::string err;
  check(drive::save_from_text(text, reloaded, &err), "DriveSave with roads/treasure_map/outposts parses");
  check(reloaded.treasure_map, "treasure_map round-trips");
  check(reloaded.road_discovery_nodes == rd.node_count(), "road_discovery_nodes round-trips");
  check(reloaded.discovered_outposts.size() == 2 && reloaded.completed_pr_stunts.size() == 2,
        "outposts & pr_stunts round-trip");

  freeroam::RoadDiscovery rd2;
  rd2.build(b.map);
  rd2.load_words(reloaded.road_discovery_nodes, reloaded.road_discovery_words);
  check(rd2.discovered_count() == 1 && rd2.is_node_discovered(a0) && !rd2.is_node_discovered(b0),
        "restored RoadDiscovery matches saved bitset");

  // Drive on Canyon Trail (both endpoints) -> 2/2.
  auto ev2a = rd2.update(300, 0);
  check(ev2a.empty(), "touching one end of 100 m Canyon Trail does not complete it");
  auto ev2 = rd2.update(300, 100);
  check(ev2.size() == 1 && ev2[0].road_name == "Canyon Trail" && ev2[0].discovered_count == 2,
        "driving full second road brings count to 2/2");

  // SettingsPage treasure map toggle.
  carview::SettingsPage sp;
  sp.load({"--treasure-map"});
  check(sp.value("treasure map") == "on", "SettingsPage reads --treasure-map as on");
  check(sp.live_settings().treasure_map, "LiveSettings reports treasure_map=true");
}

void test_unvisited_distance_threshold_75m() {
  // 1) Long highway (600 m = 6 x 100 m segments): driving 67% (400 m) leaves 200 m unvisited
  //    (> 75 m UnvisitedDistanceThreshold) and MUST NOT complete prematurely.
  // 2) Highway with final 60 m stub unvisited (<= 75 m) completes as soon as unvisited <= 75 m.
  // 3) A 79 m segment does NOT mark its far endpoint when the car is 28 m from the start node.
  NavBuilder b;
  std::vector<uint32_t> long_nodes;
  for (int i = 0; i <= 6; ++i) long_nodes.push_back(b.add_node(0.0f, float(i * 100)));
  b.add_way(long_nodes, "freeway", "I-76 Long");

  const uint32_t t0 = b.add_node(500, 0), t1 = b.add_node(500, 100), t2 = b.add_node(500, 200), t3 = b.add_node(500, 260);
  b.add_way({t0, t1, t2, t3}, "a", "Stub 60m End");

  const uint32_t p0 = b.add_node(900, 0), p1 = b.add_node(900, 79);
  b.add_way({p0, p1}, "b", "Seg 79m");

  freeroam::RoadDiscovery rd;
  check(rd.build(b.map), "RoadDiscovery builds on threshold test map");

  // Drive first 5 nodes (400 m of 600 m = 71% of nodes) on I-76 Long -> still 200 m unvisited -> NOT discovered!
  for (int i = 0; i <= 4; ++i) {
    auto ev = rd.update(0.0, double(i * 100));
    check(ev.empty(), "600 m highway must not complete with >= 200 m unvisited");
  }
  check(rd.discovered_count() == 0, "600 m highway still undiscovered at 67% length");
  check(rd.update(0.0, 500.0).empty(), "600 m highway must not complete with 100 m unvisited (> 75 m)");
  auto fin_ev = rd.update(0.0, 600.0);
  check(fin_ev.size() == 1 && fin_ev[0].road_name == "I-76 Long", "600 m highway completes once unvisited <= 75 m");

  // Stub 60m End: drive t0 -> t1 -> t2 (200 m driven, only 60 m segment t2->t3 unvisited <= 75 m) -> completes!
  check(rd.update(500.0, 0.0).empty(), "t0 alone does not complete Stub 60m End");
  check(rd.update(500.0, 100.0).empty(), "t0->t1 leaves 160 m unvisited");
  auto stub_ev = rd.update(500.0, 200.0);
  check(stub_ev.size() == 1 && stub_ev[0].road_name == "Stub 60m End",
        "road completes when remaining unvisited segment length is 60 m <= 75 m");

  // 79 m segment: at (900, 20), p0 is 20 m away (<= 28 m) while p1 is 59 m away (> 28 m).
  auto seg_ev = rd.update(900.0, 20.0, 28.0);
  check(seg_ev.empty(), "79 m segment does not complete from near start");
  check(rd.is_node_discovered(p0) && !rd.is_node_discovered(p1),
        "79 m segment marks near endpoint p0 only, never far endpoint p1");
  check(!rd.is_segment_discovered(p0, p1), "segment requires both endpoints visited");
}

void test_map_edge_clamping_internal_size_and_filters() {
  // PT-09: 4K pause map renders at capped internal resolution <= 1600x900 preserving aspect.
  const auto sz4k = carview::MapView::pause_internal_size(3840, 2071);
  check(sz4k[0] <= 1600 && sz4k[1] <= 900 && sz4k[0] >= 1200 && sz4k[1] >= 600,
        "4K pause_internal_size caps to <= 1600x900");
  const double ar_in = 3840.0 / 2071.0, ar_out = double(sz4k[0]) / double(sz4k[1]);
  check(std::fabs(ar_in - ar_out) < 0.01, "4K pause_internal_size preserves aspect ratio");
  const auto sz720 = carview::MapView::pause_internal_size(1280, 720);
  check(sz720[0] == 1280 && sz720[1] == 720, "720p pause_internal_size renders 1:1");

  // PT-32: MapCameraController keeps visible viewport strictly inside Colorado texture bounds
  // across 16:9, 4K (3840x2071), 21:9 ultrawide, and 4:3 aspect ratios at extreme pan & zoom-out.
  const std::pair<int, int> viewports[] = {{1280, 720}, {3840, 2071}, {2560, 1080}, {1280, 960}};
  const double west = freeroam::kMapWest;
  const double east = freeroam::kMapWest + freeroam::kMapWidth;
  const double south = freeroam::kMapNorth - freeroam::kMapHeight;
  const double north = freeroam::kMapNorth;
  for (const auto& [vw, vh] : viewports) {
    carview::MapCameraController cam;
    cam.set_viewport(vw, vh, nullptr);
    cam.reset(-50000.0, -50000.0, 0.25);
    check(cam.zoom() >= cam.min_zoom() - 1e-9, "camera zoom clamped >= min_zoom");
    const double max_scale = std::min((freeroam::kMapWidth * 0.996) / double(vw),
                                      (freeroam::kMapHeight * 0.996) / double(vh));
    const double scale = max_scale * (cam.min_zoom() / cam.zoom());
    const double hw = 0.5 * double(vw) * scale;
    const double hh = 0.5 * double(vh) * scale;
    check(cam.cx() - hw >= west - 1e-3 && cam.cx() + hw <= east + 1e-3,
          "camera horizontal viewport stays inside Colorado bounds (SW clamp)");
    check(cam.cz() - hh >= south - 1e-3 && cam.cz() + hh <= north + 1e-3,
          "camera vertical viewport stays inside Colorado bounds (SW clamp)");

    // Pan far NE and verify clamp + zeroed boundary velocity.
    cam.reset(50000.0, 50000.0, 3.0);
    const double scale3 = max_scale * (cam.min_zoom() / cam.zoom());
    const double hw3 = 0.5 * double(vw) * scale3;
    const double hh3 = 0.5 * double(vh) * scale3;
    check(cam.cx() + hw3 <= east + 1e-3 && cam.cz() + hh3 <= north + 1e-3,
          "camera NE clamp keeps viewport inside Colorado bounds at zoom 3x");
    cam.step(1.0 / 60.0, 1.0, 1.0, 0.0, {});
    check(cam.vx() == 0.0 && cam.vz() == 0.0, "velocity zeroes cleanly against map boundary");
  }

  // PT-34: All 7 retail World Map filter tabs classify markers accurately.
  carview::MapMarker ev, op_disc, op_lock, sp, sh, sg;
  ev.kind = carview::MapMarker::kEvent;
  ev.icon_tag = "race";
  op_disc.kind = carview::MapMarker::kOutpost;
  op_disc.icon_tag = "gas_station";
  op_disc.discovered = true;
  op_lock.kind = carview::MapMarker::kOutpost;
  op_lock.icon_tag = "gas_station";
  op_lock.discovered = false;
  sp.kind = carview::MapMarker::kSpeedTrap;
  sp.icon_tag = "speed_camera_discovered";
  sh.kind = carview::MapMarker::kEvent;
  sh.icon_tag = "exhibition";
  sg.icon_tag = "flyer";

  // The original's 12 check rows (map_filter.h): one row each, OR'd with
  // New / Not Won; facilities have no row.
  using carview::MapFilterRow;
  auto only = [](std::initializer_list<MapFilterRow> rows) {
    carview::MapFilterSet f;
    f.toggle(MapFilterRow::kAll);  // A on All unticks every row
    for (auto r : rows) f.toggle(r);
    return f;
  };
  carview::MapMarker site;
  site.kind = carview::MapMarker::kFestivalSite;
  site.icon_tag = "workshop";
  ev.completed = true, sh.completed = true;
  check(carview::map_marker_matches_filter(ev, carview::MapFilterSet{}), "all rows ticked: events show");
  check(carview::map_marker_matches_filter(ev, only({MapFilterRow::kFestivalEvents})), "festival events row: races");
  check(!carview::map_marker_matches_filter(sh, only({MapFilterRow::kFestivalEvents})),
        "festival events row: not showcases");
  check(carview::map_marker_matches_filter(sh, only({MapFilterRow::kShowcases})), "showcases row: exhibitions");
  check(carview::map_marker_matches_filter(op_disc, only({MapFilterRow::kOutposts})), "outposts row: outposts");
  check(carview::map_marker_matches_filter(sp, only({MapFilterRow::kSpeedCameras})), "speed cameras row: traps");
  check(!carview::map_marker_matches_filter(sp, only({MapFilterRow::kOutposts})), "outposts row: no traps");
  check(carview::map_marker_matches_filter(site, only({})), "facilities always show");
  carview::MapMarker fresh = ev;
  fresh.completed = false, fresh.is_new = true;
  check(carview::map_marker_matches_filter(fresh, only({MapFilterRow::kNew})), "new row: new events");
  check(carview::map_marker_matches_filter(fresh, only({MapFilterRow::kNotWon})), "not won row: unwon events");
  check(!carview::map_marker_matches_filter(ev, only({MapFilterRow::kNotWon})), "not won row: not won ones");
  {
    carview::MapFilterSet f;
    check(f.all_ticked() && f.ticked[0], "filter: every row ticked at first");
    f.toggle(MapFilterRow::kAll);
    bool none = true;
    for (bool t : f.ticked) none = none && !t;
    check(none, "filter: A on All unticks every row");
    f.toggle(MapFilterRow::kStarShowdowns);
    check(f.is_ticked(MapFilterRow::kStarShowdowns) && !f.is_ticked(MapFilterRow::kAll), "filter: one row ticked");
    for (int i = 1; i < carview::kMapFilterRows; ++i)
      if (!f.ticked[size_t(i)]) f.toggle(MapFilterRow(i));
    check(f.is_ticked(MapFilterRow::kAll), "filter: All ticks itself when every row is");
    check(std::string(carview::map_filter_string_id(MapFilterRow::kDiscountSigns)) == "IDS_Filter_Flyers" &&
              std::string(carview::map_filter_string_id(MapFilterRow::kStarShowdowns)) == "IDS_Filter_NemesisRaces",
          "filter: the rows' InGame ids");
  }
  {
    // The free cursor: moves over a still map, scrolls past the edge zone.
    carview::MapCursor c;
    auto s = c.step(0.1, 1.0f, 0.0f);
    check(std::abs(c.x - 689.0f) < 0.5f && s[0] == 0.0f, "cursor: 490 px/s, no scroll inside the zone");
    for (int i = 0; i < 10; ++i) s = c.step(0.1, 1.0f, 0.0f);
    check(std::abs(c.x - 830.0f) < 0.5f && std::abs(s[0] - 47.0f) < 0.5f, "cursor: held at 190 px, scrolls 470 px/s");
    c.absorb(1000.0f, 0.0f);
    check(c.x == carview::MapCursor::kMaxX, "cursor: what the map cannot take moves it to the edge");
    s = c.step(0.1, 0.0f, -1.0f, true);
    check(std::abs(c.y - (360.0f - 22.05f)) < 0.5f, "cursor: slower over an icon");
    const double z0 = 2.0;
    double z = z0;
    for (int i = 0; i < 60; ++i) z = carview::MapZoom::step(z, z0, 1.0, 1.0 / 60);
    check(std::abs(z - z0 * carview::MapZoom::kMaxIn) < 1e-9, "zoom: RS up to 2.6x within a second");
    check(carview::MapZoom::step(z0, z0, -1.0, 1.0) == z0, "zoom: the opening scale is the furthest out");
  }
}

void test_map_icon_status_layers() {
  using carview::MapIconState;
  using carview::MapView;
  using Role = MapView::IconRole;

  // 1. Role classification across both fullscreen (icon_up_*) and minimap (icon_facing_*) symbolizers.
  check(MapView::classify_icon_role("icon_up", "race", 0, 2) == Role::kBase, "race shadow -> kBase");
  check(MapView::classify_icon_role("icon_up_career", "race", 1, 0) == Role::kCareerBase, "race icon_up_career -> kCareerBase");
  check(MapView::classify_icon_role("icon_facing_career", "race", 1, 0) == Role::kCareerBase, "race icon_facing_career -> kCareerBase");
  check(MapView::classify_icon_role("icon_up_career_colour", "race", 0, 0) == Role::kCareerColour, "race colour -> kCareerColour");
  check(MapView::classify_icon_role("icon_up_career_finish", "race", 5, 1) == Role::kCareerFinish, "race finish -> kCareerFinish");
  check(MapView::classify_icon_role("icon_up_career_note", "race", 6, 1) == Role::kCareerNote, "race note -> kCareerNote");
  check(MapView::classify_icon_role("icon_up", "nemesisrace", 4, 1) == Role::kCareerBase, "nemesis fullscreen (4,1) -> kCareerBase");
  check(MapView::classify_icon_role("icon_facing", "nemesisrace", 4, 1) == Role::kCareerBase, "nemesis minimap (4,1) -> kCareerBase");
  check(MapView::classify_icon_role("icon_facing", "streetrace", 2, 1) == Role::kCareerBase, "streetrace minimap (2,1) -> kCareerBase");
  check(MapView::classify_icon_role("icon_up", "gas_station", 1, 2) == Role::kBase, "gas_station -> kBase");

  MapView::IconStyle s_shadow{0, 2, 45, {255, 255, 255, 255}, Role::kBase, 520, {}, 0, 0};
  MapView::IconStyle s_base{1, 0, 45, {255, 255, 255, 255}, Role::kCareerBase, 530, {}, 0, 0};
  MapView::IconStyle s_ring{0, 0, 45, {255, 255, 255, 255}, Role::kCareerColour, 535, {}, 0, 0};
  MapView::IconStyle s_finish{5, 1, 45, {255, 255, 255, 255}, Role::kCareerFinish, 540, {}, 0, 0};
  MapView::IconStyle s_note{6, 1, 45, {255, 255, 255, 255}, Role::kCareerNote, 550, {}, 0, 0};

  // 2. Uncompleted eligible P2P green wristband race: base (1,0), green ring, NO trophy, NO '!', HAS NEW.
  {
    MapIconState st;
    st.completed = false;
    st.eligible = true;
    st.wristband_level = 1;  // Green
    const auto r_base = MapView::resolve_icon_layer(s_base, "race", st);
    const auto r_ring = MapView::resolve_icon_layer(s_ring, "race", st);
    const auto r_fin = MapView::resolve_icon_layer(s_finish, "race", st);
    const auto r_note = MapView::resolve_icon_layer(s_note, "race", st);
    check(r_base.visible && r_base.cell_x == 1 && r_base.cell_y == 0 && r_base.colour.a == 255.0f,
          "uncompleted P2P draws opaque (1,0) base");
    check(r_ring.visible && r_ring.cell_x == 0 && r_ring.cell_y == 0 && r_ring.colour.g > 180.0f && r_ring.colour.r < 90.0f,
          "uncompleted Green race tints ring green");
    check(!r_fin.visible, "uncompleted event NEVER shows trophy cup (5,1)");
    check(!r_note.visible, "eligible event NEVER shows '!' warning badge (6,1)");
    check(MapView::marker_shows_new_badge("race", true, false), "uncompleted new race shows NEW");
  }

  // 3. Uncompleted ineligible race: base + wristband ring + '!' (6,1), NO trophy cup (5,1).
  {
    MapIconState st;
    st.completed = false;
    st.eligible = false;
    st.wristband_level = 2;  // Blue
    const auto r_ring = MapView::resolve_icon_layer(s_ring, "race", st);
    const auto r_fin = MapView::resolve_icon_layer(s_finish, "race", st);
    const auto r_note = MapView::resolve_icon_layer(s_note, "race", st);
    check(r_ring.visible && r_ring.colour.b > 200.0f, "uncompleted Blue race tints ring blue");
    check(!r_fin.visible, "uncompleted ineligible race has NO trophy cup");
    check(r_note.visible && r_note.cell_x == 6 && r_note.cell_y == 1 && r_note.colour.a == 255.0f,
          "ineligible race draws opaque '!' warning badge (6,1)");
  }

  // 4. Completed event (1st / 2nd / 3rd): muted opaque base, opaque dark grey ring, gold/silver/bronze trophy, NO NEW.
  for (int place : {1, 2, 3}) {
    MapIconState st;
    st.completed = true;
    st.best_place = place;
    st.eligible = true;
    st.wristband_level = 1;
    const auto r_sh = MapView::resolve_icon_layer(s_shadow, "race", st);
    const auto r_base = MapView::resolve_icon_layer(s_base, "race", st);
    const auto r_ring = MapView::resolve_icon_layer(s_ring, "race", st);
    const auto r_fin = MapView::resolve_icon_layer(s_finish, "race", st);
    const auto r_note = MapView::resolve_icon_layer(s_note, "race", st);
    check(r_sh.visible && r_sh.colour.a == 255.0f, "completed shadow stays 100% opaque");
    check(r_base.visible && r_base.colour.a == 255.0f && r_base.colour.r < 220.0f && r_base.colour.r > 180.0f,
          "completed base stays 100% opaque with muted RGB");
    check(r_ring.visible && r_ring.cell_x == 0 && r_ring.cell_y == 0 && r_ring.colour.a == 255.0f &&
              r_ring.colour.r == 100.0f && r_ring.colour.g == 102.0f && r_ring.colour.b == 108.0f,
          "completed wristband ring turns opaque dark grey");
    check(r_fin.visible && r_fin.cell_x == 5 && r_fin.cell_y == 1 && r_fin.colour.a == 255.0f,
          "completed event shows opaque trophy cup (5,1)");
    if (place == 1) check(r_fin.colour.r == 255.0f && r_fin.colour.g == 255.0f && r_fin.colour.b == 255.0f, "1st -> gold");
    if (place == 2) check(r_fin.colour.r == 215.0f && r_fin.colour.g == 222.0f && r_fin.colour.b == 232.0f, "2nd -> silver");
    if (place == 3) check(r_fin.colour.r == 218.0f && r_fin.colour.g == 152.0f && r_fin.colour.b == 96.0f, "3rd -> bronze");
    check(!r_note.visible, "completed eligible event has NO '!' badge");
    st.eligible = false;
    check(!MapView::resolve_icon_layer(s_note, "race", st).visible,
          "completed event suppresses '!' badge even when current car is ineligible");
    check(!MapView::marker_shows_new_badge("race", true, true), "completed event NEVER shows NEW");
  }

  // 5. Circuit (2,0), Finale (7,1), and Gold wristband metallic ring (7,0) / nemesis star (4,5).
  {
    MapIconState circ;
    circ.is_circuit = true;
    circ.wristband_level = 6;  // Gold
    const auto r_cb = MapView::resolve_icon_layer(s_base, "race", circ);
    const auto r_cr = MapView::resolve_icon_layer(s_ring, "race", circ);
    check(r_cb.cell_x == 2 && r_cb.cell_y == 0, "circuit race selects oval+flag cell (2,0)");
    check(r_cr.cell_x == 7 && r_cr.cell_y == 0 && r_cr.colour.r == 255.0f && r_cr.colour.g == 255.0f && r_cr.colour.b == 255.0f,
          "Gold wristband race selects gold metallic ring (7,0) untinted");

    MapIconState fin;
    fin.is_circuit = true;
    fin.is_finale = true;
    fin.wristband_level = 6;
    const auto r_fb = MapView::resolve_icon_layer(s_base, "race", fin);
    check(r_fb.cell_x == 7 && r_fb.cell_y == 1, "Horizon Finale selects crown cell (7,1)");

    MapView::IconStyle s_nem_ring{5, 0, 75, {255, 255, 255, 255}, Role::kCareerColour, 535, {}, 0, 0};
    MapIconState nem_gold;
    nem_gold.wristband_level = 6;
    const auto r_nr = MapView::resolve_icon_layer(s_nem_ring, "nemesisrace", nem_gold);
    check(r_nr.cell_x == 4 && r_nr.cell_y == 5 && r_nr.colour.r == 255.0f,
          "Gold wristband nemesis selects gold metallic star (4,5) untinted");
    nem_gold.completed = true;
    const auto r_nr_done = MapView::resolve_icon_layer(s_nem_ring, "nemesisrace", nem_gold);
    check(r_nr_done.cell_x == 5 && r_nr_done.cell_y == 0 && r_nr_done.colour.r == 100.0f && r_nr_done.colour.a == 255.0f,
          "completed Gold nemesis reverts to opaque dark grey star (5,0)");
  }

  // 6. Street hub partial vs 100% completion, and non-career NEW suppression + full opacity on completed speed traps.
  {
    MapView::IconStyle s_st_fin{5, 1, 45, {255, 255, 255, 255}, Role::kCareerFinish, 540, {}, 0, 0};
    MapIconState partial;
    partial.completed = false;
    check(!MapView::resolve_icon_layer(s_st_fin, "streetrace", partial).visible,
          "partially completed street hub has NO trophy cup");
    MapIconState full;
    full.completed = true;
    full.best_place = 1;
    check(MapView::resolve_icon_layer(s_st_fin, "streetrace", full).visible,
          "100% completed street hub HAS trophy cup");

    MapView::IconStyle s_trap{0, 3, 45, {255, 255, 255, 255}, Role::kBase, 350, {}, 0, 0};
    MapIconState trap_done;
    trap_done.completed = true;
    const auto r_trap = MapView::resolve_icon_layer(s_trap, "speed_camera_completed", trap_done);
    check(r_trap.visible && r_trap.colour.a == 255.0f, "completed speed trap stays 100% opaque");
    check(!MapView::marker_shows_new_badge("speed_camera_discovered", true, false), "speed trap never shows NEW");
    check(!MapView::marker_shows_new_badge("average_speed_camera_discovered", true, false), "speed zone never shows NEW");
    check(!MapView::marker_shows_new_badge("gas_station", true, false), "outpost never shows NEW");
  }
}

void test_colorado_corpus_optional() {
  const char* gr = std::getenv("SHIFT_GAME_ROOT");
  if (!gr || !*gr) return;
  const std::filesystem::path game_root(gr);
  const auto nav_path = game_root / "media" / "tracks" / "colorado" / "colorado.nav";
  if (!std::filesystem::exists(nav_path)) return;

  traffic::NavMap nav;
  std::string err;
  check(traffic::read_nav(nav_path, nav, &err), "read colorado.nav");

  freeroam::RoadDiscovery rd;
  check(rd.build(nav), "build RoadDiscovery on Colorado");
  check(rd.total_roads() == 216, "Colorado has exactly 216 discoverable roads");
  check(rd.node_count() == 12036, "Colorado has 12036 nav nodes");
  check(rd.words().size() == 377, "CRoadDiscovery bitset is 377 uint32 words");

  // If out/profile/user exists, verify ForzaProfile CRoadDiscovery & CGasStationState import.
  const std::filesystem::path prof_dir("out/profile/user");
  if (std::filesystem::exists(prof_dir / "ForzaProfile.body")) {
    profile::Imported imp;
    check(profile::import_profile(prof_dir, imp, &err), "import reference ForzaProfile");
    check(imp.road_discovery_nodes == 12036 && imp.road_discovery_words.size() == 377,
          "ForzaProfile CRoadDiscovery decoded 12036 nodes / 377 words");
    check(imp.outposts_discovered.size() == 10, "ForzaProfile CGasStationState decoded 10 outposts");
    rd.load_words(imp.road_discovery_nodes, imp.road_discovery_words);
    check(rd.discovered_count() == 216, "reference ForzaProfile maps to 216/216 discovered roads");
    std::printf("ForzaProfile import check: %d / %d roads discovered, %zu outposts\n", rd.discovered_count(),
                rd.total_roads(), imp.outposts_discovered.size());
  }

  // Verify MapView loads high-res 15-tile pyramid lazily when zoomed in.
  carview::MapView mv;
  check(mv.open(game_root, &err), "MapView opens Colorado map assets");
  check(mv.loaded_high_tiles() == 0, "high-res tiles lazy-loaded (0 at startup)");
  carview::MapScene sc;
  sc.x = -610.0;
  sc.z = -255.0;
  sc.road_discovery = &rd;
  const auto img = mv.pause_map(sc, 640, 360, 5.0, sc.x, sc.z);
  check(img.width == 640 && img.height == 360, "high-res pause_map renders cleanly");
  check(mv.loaded_high_tiles() > 0, "high-res tiles decoded on zoomed render");
  std::printf("MapView high-res pyramid check: %d tile(s) cached at zoom 5x\n", mv.loaded_high_tiles());

  // PT-32: Extreme SW pan + zoomed-out 4K render produces ZERO solid black void border pixels.
  {
    const auto edge_img = mv.pause_map(sc, 1600, 900, 0.5, -25000.0, -25000.0);
    int void_px = 0;
    for (int y : {0, 4, 895, 899}) {
      for (int x = 0; x < 1600; x += 16) {
        const uint8_t* p = &edge_img.rgba[(size_t(y) * 1600 + size_t(x)) * 4];
        if (p[0] == 12 && p[1] == 12 && p[2] == 14) ++void_px;
      }
    }
    for (int x : {0, 4, 1595, 1599}) {
      for (int y = 0; y < 900; y += 16) {
        const uint8_t* p = &edge_img.rgba[(size_t(y) * 1600 + size_t(x)) * 4];
        if (p[0] == 12 && p[1] == 12 && p[2] == 14) ++void_px;
      }
    }
    check(void_px == 0, "PT-32: extreme SW pan has zero black border void pixels");
    // Nor the background texture's own black margins (a band of black down a
    // whole side of the view); the art's own shadows and lakes are dark in places.
    for (const auto& [ex, ez] : {std::pair{-25000.0, 0.0}, std::pair{25000.0, 0.0}, std::pair{0.0, 25000.0},
                                 std::pair{0.0, -25000.0}}) {
      const auto img = mv.pause_map(sc, 1280, 720, 2.0, ex, ez);
      auto dark = [&](int x, int y) {
        const uint8_t* p = &img.rgba[(size_t(y) * 1280 + size_t(x)) * 4];
        return std::max({p[0], p[1], p[2]}) <= 40 ? 1 : 0;
      };
      int left = 0, right = 0, top = 0, bottom = 0;
      for (int y = 0; y < 720; y += 8) left += dark(2, y), right += dark(1277, y);
      for (int x = 0; x < 1280; x += 8) top += dark(x, 2), bottom += dark(x, 717);
      check(std::max(left, right) * 2 < 90 && std::max(top, bottom) * 2 < 160,
            "the pause map's camera stays off the background's black margins");
    }
  }

  // PT-33: Verify Horizon Outpost "H" icon (atlas cell 1,2) has valid RGBA and renders
  // with undiscovered tint without signed-32-bit overflow electric-blue noise speckles.
  {
    const auto& atlas = mv.large_icons();
    check(!atlas.rgba.empty() && atlas.width >= 256 && atlas.height >= 256, "MapIconSheet decoded");
    const uint32_t cell = atlas.width / 8;
    int opaque_px = 0;
    for (uint32_t y = 2 * cell; y < 3 * cell; ++y) {
      for (uint32_t x = 1 * cell; x < 2 * cell; ++x) {
        const uint8_t* p = &atlas.rgba[(size_t(y) * atlas.width + x) * 4];
        if (p[3] > 200) ++opaque_px;
      }
    }
    check(opaque_px > 100, "PT-33: Horizon 'H' icon atlas cell (1,2) has valid opaque pixels");

    carview::MapScene icon_sc = sc;
    carview::MapMarker op;
    op.kind = carview::MapMarker::kOutpost;
    op.icon_tag = "gas_station";
    op.id = "GasStation_Test";
    op.x = -610.0;
    op.z = -255.0;
    op.discovered = false;
    icon_sc.markers.push_back(op);
    const auto icon_img = mv.pause_map(icon_sc, 640, 360, 6.0, op.x, op.z);
    int blue_corrupt_px = 0;
    for (int y = 180 - 36; y <= 180 + 36; ++y) {
      for (int x = 320 - 36; x <= 320 + 36; ++x) {
        const uint8_t* p = &icon_img.rgba[(size_t(y) * 640 + size_t(x)) * 4];
        // Signed 32-bit overflow produced electric-blue pixels (B > 180 with R < 50) inside the greyscale-tinted "H".
        if (int(p[2]) > 180 && int(p[2]) - int(p[0]) > 110) ++blue_corrupt_px;
      }
    }
    check(blue_corrupt_px == 0, "PT-33: undiscovered Horizon 'H' icon renders cleanly with zero overflow artifacts");

    // The profile's animations: the player's looping radar ping, the
    // facilities' intro grow, the barn rumour camera moves.
    const auto* radar = mv.icon_animation("player_local", "default");
    check(radar && radar->name == "radar" && radar->loop == carview::mapanim::kLoop &&
              std::fabs(radar->duration - 3.0f) < 1e-4f,
          "MapProfileFullscreen: player_local pings with the looping 3 s radar");
    const auto* grow = mv.icon_animation("racecentral", "intro");
    check(grow && grow->name == "grow", "MapProfileFullscreen: racecentral's intro is grow");
    const auto* barn1 = mv.animation("barn_find_region1_unlock");
    check(barn1 && std::fabs(carview::mapanim::camera_at(*barn1, 10).x - 1310) < 1e-3 &&
              std::fabs(carview::mapanim::camera_at(*barn1, 10).z + 4921) < 1e-3,
          "MapProfileFullscreen: barn_find_region1_unlock moves the camera to (1310, -4921)");
    // Mid-ping (1.5 s: a ring at about 4x, alpha 0.25) differs from before
    // it (0.5 s: scale 0) away from the arrow.
    carview::MapScene ping_sc = sc;
    ping_sc.anim_time = 0.5;
    const auto before_ping = mv.pause_map(ping_sc, 640, 360, 6.0, sc.x, sc.z);
    ping_sc.anim_time = 1.5;
    const auto mid_ping = mv.pause_map(ping_sc, 640, 360, 6.0, sc.x, sc.z);
    int ring_px = 0;
    for (int y = 0; y < 360; ++y)
      for (int x = 0; x < 640; ++x) {
        if (std::hypot(x - 320.0, y - 180.0) < 30) continue;
        const size_t o = (size_t(y) * 640 + size_t(x)) * 4;
        if (std::abs(int(mid_ping.rgba[o]) - int(before_ping.rgba[o])) > 8) ++ring_px;
      }
    check(ring_px > 200, "the radar ping's ring shows mid-pass");

    // Each rumour reveal ends with the rumour's circle in view: the camera
    // on the animation's target at its zoom fraction (zoom ~ 1 / distance
    // from the opening's Zoom_Default distance), at 1280x720.
    freeroam::Collectibles col;
    if (freeroam::load_collectibles(game_root, col, &err) && col.barns.size() == 9) {
      const double base = std::max(2.0, mv.min_zoom(1280, 720));
      const double d_open = carview::mapanim::map_distance(carview::mapanim::kZoomDefault);
      int in_view = 0;
      for (const auto& b : col.barns) {
        const auto* a = mv.animation(b.map_animation);
        if (!a) continue;
        const auto m = carview::mapanim::camera_at(*a, 100);
        const double zoom = base * d_open / carview::mapanim::map_distance(m.zoom);
        double cx = m.x, cz = m.z;
        mv.clamp_camera(1280, 720, zoom, cx, cz);
        const double mpp = mv.map_scale(1280, 720, zoom);
        const double hx = 640 * mpp, hz = 360 * mpp;
        const double dx = std::max(0.0, std::fabs(b.hint_x - cx) - hx), dz = std::max(0.0, std::fabs(b.hint_z - cz) - hz);
        const bool centre_in = std::fabs(b.hint_x - cx) <= hx && std::fabs(b.hint_z - cz) <= hz;
        std::printf("reveal %s: zoom %.2fx, view %.0f x %.0f m, circle centre %s (%.0f m off)\n", b.name.c_str(),
                    zoom / base, 2 * hx, 2 * hz, centre_in ? "in view" : "out", std::hypot(dx, dz));
        in_view += std::hypot(dx, dz) < b.hint_rx;
      }
      check(in_view == 9, "every barn rumour reveal ends with its circle in view");
    }
  }

  // PT-35: Minimap at bug_001 (2303.92, 548.02) and bug_023 (-3025.10, -1727.87) with active
  // green GPS route AND fresh undiscovered roads must show BOTH grey/white road shoulders and dark casing.
  {
    freeroam::RoadDiscovery fresh_rd;
    fresh_rd.build(nav);
    traffic::GpsSolver solver;
    solver.build(nav);
    const struct {
      const char* tag;
      double x, z, hdg, spd;
    } cases[] = {
        {"bug_001", 2303.92, 548.02, -3.0964, 5.0},
        {"bug_023", -3025.10, -1727.87, -0.35, 37.5},
    };
    for (const auto& tc : cases) {
      carview::MapScene msc;
      msc.x = tc.x;
      msc.z = tc.z;
      msc.heading = tc.hdg;
      msc.speed_mps = tc.spd;
      msc.road_discovery = &fresh_rd;
      const auto r = solver.solve(tc.x, tc.z, -610.0, -255.0);
      for (const auto& pt : r.points) msc.gps_route.push_back({pt.x, pt.z});
      const auto mm = mv.minimap(msc, 256);
      int road_grey_px = 0, casing_dark_px = 0, gps_green_px = 0;
      for (size_t i = 0; i < mm.rgba.size(); i += 4) {
        const int r0 = mm.rgba[i], g0 = mm.rgba[i + 1], b0 = mm.rgba[i + 2], a0 = mm.rgba[i + 3];
        if (a0 < 80) continue;
        if (g0 > r0 + 55 && g0 > b0 + 40) {
          ++gps_green_px;
        } else if (r0 >= 135 && g0 >= 135 && b0 >= 135 && std::abs(r0 - g0) < 35) {
          ++road_grey_px;
        } else if (r0 < 45 && g0 < 45 && b0 < 50 && a0 >= 120) {
          ++casing_dark_px;
        }
      }
      check(gps_green_px > 40, "PT-35: minimap renders green GPS route at playtest coords");
      check(road_grey_px > 80, "PT-35: minimap renders visible grey/white road ribbon underneath GPS route");
      check(casing_dark_px > 60, "PT-35: minimap renders dark road outline casing underneath GPS route");
      std::printf("PT-35 minimap check (%s): green=%d road_grey=%d casing=%d\n",
                  tc.tag, gps_green_px, road_grey_px, casing_dark_px);
    }
  }

  // The minimap's camera is fixed (docs/UI_SCENE_SCREENS.md, Q89): the same
  // frame at rest and at 60 m/s.
  {
    carview::MapScene msc;
    msc.x = 2303.92, msc.z = 548.02, msc.heading = -3.0964;
    msc.speed_mps = 0.0;
    const auto still = mv.minimap(msc, 180);
    msc.speed_mps = 60.0;
    const auto fast = mv.minimap(msc, 180);
    check(still.width == 180 && still.rgba == fast.rgba, "minimap: the same view at rest and at 60 m/s");
  }

  // PT-09: Cached 4K pause_map panning stays fast (zero 4K base re-rasterization on pan cache hits).
  {
    const auto prime = mv.pause_map(sc, 3840, 2071, 3.0, -610.0, -255.0);
    check(prime.width == 3840 && prime.height == 2071, "PT-09: 4K pause_map outputs full 3840x2071 frame");
    // Second call at steady zoom populates the padded internal base cache.
    (void)mv.pause_map(sc, 3840, 2071, 3.0, -600.0, -255.0);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kPanFrames = 8;
    for (int f = 0; f < kPanFrames; ++f) {
      const auto p = mv.pause_map(sc, 3840, 2071, 3.0, -600.0 + double(f + 1) * 18.0, -255.0);
      check(p.width == 3840 && p.height == 2071, "PT-09: cached 4K pan frame dimensions match");
    }
    const double avg_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / double(kPanFrames);
    std::printf("PT-09 cached 4K pause_map pan average: %.2f ms/frame\n", avg_ms);
    check(avg_ms < 40.0, "PT-09: cached 4K pause_map panning stays well under 40 ms/frame even unoptimized");
  }

  // Bug 4: 1:1 map event status icons across BOTH pause_map() and minimap() on real retail XML + MapIconSheet.
  {
    using Role = carview::MapView::IconRole;
    const auto* full_race = mv.fullscreen_icon_styles("race");
    const auto* mini_race = mv.minimap_icon_styles("race");
    check(full_race && full_race->size() == 5, "MapProfileFullscreen race has 5 classified symbolizers");
    check(mini_race && mini_race->size() == 4, "MapProfileMinimap race has 4 classified symbolizers");
    if (full_race && full_race->size() == 5) {
      check((*full_race)[0].role == Role::kBase && (*full_race)[1].role == Role::kCareerBase &&
                (*full_race)[2].role == Role::kCareerColour && (*full_race)[3].role == Role::kCareerFinish &&
                (*full_race)[4].role == Role::kCareerNote,
            "MapProfileFullscreen race symbolizers sorted: shadow -> base -> ring -> trophy -> note");
    }
    if (mini_race && mini_race->size() == 4) {
      check((*mini_race)[0].role == Role::kCareerBase && (*mini_race)[1].role == Role::kCareerColour &&
                (*mini_race)[2].role == Role::kCareerFinish && (*mini_race)[3].role == Role::kCareerNote,
            "MapProfileMinimap race symbolizers sorted: base -> ring -> trophy -> note");
    }

    auto diff_region = [](const carview::HudImage& a, const carview::HudImage& b, int x0, int y0, int x1, int y1) {
      int n = 0;
      const int w = int(a.width), h = int(a.height);
      x0 = std::clamp(x0, 0, w - 1);
      x1 = std::clamp(x1, 0, w - 1);
      y0 = std::clamp(y0, 0, h - 1);
      y1 = std::clamp(y1, 0, h - 1);
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const size_t o = (size_t(y) * size_t(w) + size_t(x)) * 4;
          const int dr = std::abs(int(a.rgba[o]) - int(b.rgba[o]));
          const int dg = std::abs(int(a.rgba[o + 1]) - int(b.rgba[o + 1]));
          const int db = std::abs(int(a.rgba[o + 2]) - int(b.rgba[o + 2]));
          if (dr + dg + db > 45) ++n;
        }
      return n;
    };
    auto count_gold_diff = [](const carview::HudImage& base, const carview::HudImage& with_badge, int x0, int y0, int x1,
                              int y1) {
      int n = 0;
      const int w = int(base.width), h = int(base.height);
      x0 = std::clamp(x0, 0, w - 1);
      x1 = std::clamp(x1, 0, w - 1);
      y0 = std::clamp(y0, 0, h - 1);
      y1 = std::clamp(y1, 0, h - 1);
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const size_t o = (size_t(y) * size_t(w) + size_t(x)) * 4;
          const int r = with_badge.rgba[o], g = with_badge.rgba[o + 1], b = with_badge.rgba[o + 2];
          const int dr = r - int(base.rgba[o]);
          const int dg = g - int(base.rgba[o + 1]);
          if (r > 170 && g > 130 && b < 110 && (dr > 30 || dg > 30)) ++n;
        }
      return n;
    };
    auto count_pink = [](const carview::HudImage& img, int x0, int y0, int x1, int y1) {
      int n = 0;
      const int w = int(img.width), h = int(img.height);
      x0 = std::clamp(x0, 0, w - 1);
      x1 = std::clamp(x1, 0, w - 1);
      y0 = std::clamp(y0, 0, h - 1);
      y1 = std::clamp(y1, 0, h - 1);
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const size_t o = (size_t(y) * size_t(w) + size_t(x)) * 4;
          const int r = img.rgba[o], g = img.rgba[o + 1], b = img.rgba[o + 2];
          if (r > 180 && g < 80 && b > 70) ++n;
        }
      return n;
    };

    // Pause map pixel verification (marker centered at screen (320, 180); player placed far away).
    const double mx = 0.0, mz = 0.0;
    auto make_pause_sc = [&](const carview::MapMarker* mk) {
      carview::MapScene s;
      s.x = mx + 5000.0;
      s.z = mz + 5000.0;
      if (mk) s.markers.push_back(*mk);
      return s;
    };
    const auto pause_empty = mv.pause_map(make_pause_sc(nullptr), 640, 360, 6.0, mx, mz);

    carview::MapMarker ev_uncomp;
    ev_uncomp.kind = carview::MapMarker::kEvent;
    ev_uncomp.icon_tag = "race";
    ev_uncomp.id = "TEST_RACE";
    ev_uncomp.x = mx;
    ev_uncomp.z = mz;
    ev_uncomp.is_new = false;
    ev_uncomp.completed = false;
    ev_uncomp.eligible = true;
    ev_uncomp.wristband_level = 1;  // Green

    carview::MapMarker ev_inelig = ev_uncomp;
    ev_inelig.eligible = false;

    carview::MapMarker ev_comp1 = ev_uncomp;
    ev_comp1.completed = true;
    ev_comp1.best_place = 1;
    ev_comp1.is_new = true;  // Must be suppressed because completed == true!

    carview::MapMarker ev_new = ev_uncomp;
    ev_new.is_new = true;

    carview::MapMarker ev_circuit = ev_uncomp;
    ev_circuit.is_circuit = true;

    carview::MapMarker ev_finale = ev_uncomp;
    ev_finale.is_finale = true;
    ev_finale.wristband_level = 6;

    const auto p_uncomp = mv.pause_map(make_pause_sc(&ev_uncomp), 640, 360, 6.0, mx, mz);
    const auto p_inelig = mv.pause_map(make_pause_sc(&ev_inelig), 640, 360, 6.0, mx, mz);
    const auto p_comp1 = mv.pause_map(make_pause_sc(&ev_comp1), 640, 360, 6.0, mx, mz);
    const auto p_new = mv.pause_map(make_pause_sc(&ev_new), 640, 360, 6.0, mx, mz);
    const auto p_circ = mv.pause_map(make_pause_sc(&ev_circuit), 640, 360, 6.0, mx, mz);
    const auto p_fin = mv.pause_map(make_pause_sc(&ev_finale), 640, 360, 6.0, mx, mz);

    check(count_gold_diff(pause_empty, p_uncomp, 275, 135, 308, 175) == 0,
          "pause_map: uncompleted eligible event draws ZERO gold trophy cup pixels");
    check(count_gold_diff(pause_empty, p_comp1, 275, 135, 308, 175) > 25,
          "pause_map: completed 1st-place event draws gold trophy cup pixels at top-left");
    check(count_gold_diff(pause_empty, p_uncomp, 336, 148, 365, 190) == 0,
          "pause_map: eligible event draws ZERO yellow '!' warning pixels");
    check(count_gold_diff(p_uncomp, p_inelig, 336, 148, 365, 190) > 20,
          "pause_map: ineligible event draws yellow '!' warning pixels at right");
    check(count_pink(p_new, 292, 122, 348, 158) > 35,
          "pause_map: uncompleted is_new event draws pink NEW badge");
    check(count_pink(p_comp1, 292, 122, 348, 158) == 0,
          "pause_map: completed event suppresses pink NEW badge even when is_new=true");
    check(diff_region(p_uncomp, p_circ, 304, 164, 336, 196) > 30,
          "pause_map: circuit race icon (2,0) differs from P2P (1,0)");
    check(diff_region(p_uncomp, p_fin, 304, 164, 336, 196) > 30,
          "pause_map: Horizon Finale crown (7,1) differs from P2P (1,0)");

    carview::MapMarker hub_partial;
    hub_partial.kind = carview::MapMarker::kStreetHub;
    hub_partial.icon_tag = "streetrace";
    hub_partial.id = "HUB_1";
    hub_partial.x = mx;
    hub_partial.z = mz;
    hub_partial.completed = false;
    hub_partial.is_new = false;
    carview::MapMarker hub_done = hub_partial;
    hub_done.completed = true;
    hub_done.best_place = 1;
    const auto p_hub_part = mv.pause_map(make_pause_sc(&hub_partial), 640, 360, 6.0, mx, mz);
    const auto p_hub_done = mv.pause_map(make_pause_sc(&hub_done), 640, 360, 6.0, mx, mz);
    check(count_gold_diff(pause_empty, p_hub_part, 275, 135, 308, 175) == 0,
          "pause_map: partially completed street hub draws ZERO gold trophy pixels");
    check(count_gold_diff(p_hub_part, p_hub_done, 275, 135, 308, 175) > 25,
          "pause_map: 100% completed street hub draws gold trophy pixels");

    // Minimap pixel verification: marker 150 m directly ahead of the player (centered horizontally at x=128).
    auto make_mini_sc = [&](const carview::MapMarker* mk) {
      carview::MapScene s;
      s.x = mx;
      s.z = mz - 150.0;
      s.heading = 0.0;
      if (mk) s.markers.push_back(*mk);
      return s;
    };
    const auto m_empty = mv.minimap(make_mini_sc(nullptr), 256);
    const auto m_uncomp = mv.minimap(make_mini_sc(&ev_uncomp), 256);
    const auto m_inelig = mv.minimap(make_mini_sc(&ev_inelig), 256);
    const auto m_comp1 = mv.minimap(make_mini_sc(&ev_comp1), 256);
    const auto m_new = mv.minimap(make_mini_sc(&ev_new), 256);
    const auto m_hub_part = mv.minimap(make_mini_sc(&hub_partial), 256);
    const auto m_hub_done = mv.minimap(make_mini_sc(&hub_done), 256);

    check(count_gold_diff(m_empty, m_uncomp, 70, 50, 128, 130) == 0,
          "minimap: uncompleted eligible event draws ZERO gold trophy cup pixels");
    check(count_gold_diff(m_empty, m_comp1, 70, 50, 128, 130) > 15,
          "minimap: completed 1st-place event draws gold trophy cup pixels");
    check(count_gold_diff(m_empty, m_uncomp, 132, 55, 185, 130) == 0,
          "minimap: eligible event draws ZERO yellow '!' warning pixels");
    check(count_gold_diff(m_uncomp, m_inelig, 132, 55, 185, 130) > 12,
          "minimap: ineligible event draws yellow '!' warning pixels");
    check(count_pink(m_new, 90, 35, 166, 105) > 20,
          "minimap: uncompleted is_new event draws pink NEW badge");
    check(count_pink(m_comp1, 90, 35, 166, 105) == 0,
          "minimap: completed event suppresses pink NEW badge");
    check(count_gold_diff(m_empty, m_hub_part, 70, 50, 128, 130) == 0,
          "minimap: partially completed street hub draws ZERO gold trophy pixels");
    check(count_gold_diff(m_hub_part, m_hub_done, 70, 50, 128, 130) > 15,
          "minimap: 100% completed street hub draws gold trophy pixels");
  }
}

}  // namespace

int main() {
  test_synthetic_discovery_and_save();
  test_unvisited_distance_threshold_75m();
  test_map_edge_clamping_internal_size_and_filters();
  test_map_icon_status_layers();
  test_colorado_corpus_optional();
  if (g_failed == 0) std::printf("road_discovery_tests: PASS\n");
  return g_failed ? 1 : 0;
}
