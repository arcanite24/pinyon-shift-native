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
}

}  // namespace

int main() {
  test_synthetic_discovery_and_save();
  test_unvisited_distance_threshold_75m();
  test_map_edge_clamping_internal_size_and_filters();
  test_colorado_corpus_optional();
  if (g_failed == 0) std::printf("road_discovery_tests: PASS\n");
  return g_failed ? 1 : 0;
}
