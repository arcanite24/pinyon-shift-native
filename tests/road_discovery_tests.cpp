#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "drive/save.h"
#include "freeroam/road_discovery.h"
#include "map_view.h"
#include "profile/drive_save.h"
#include "profile/profile.h"
#include "settings_page.h"
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
}

}  // namespace

int main() {
  test_synthetic_discovery_and_save();
  test_unvisited_distance_threshold_75m();
  test_colorado_corpus_optional();
  if (g_failed == 0) std::printf("road_discovery_tests: PASS\n");
  return g_failed ? 1 : 0;
}
