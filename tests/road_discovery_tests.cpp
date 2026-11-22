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

  auto ev1 = rd.update(0, 50);
  check(ev1.size() == 1 && ev1[0].road_name == "Highway 1" && ev1[0].discovered_count == 1 && ev1[0].total_count == 2,
        "driving on Highway 1 discovers road 1/2");
  check(rd.is_node_discovered(a0) && rd.is_node_discovered(a1), "segment endpoints marked discovered");
  check(!rd.is_node_discovered(b0), "other road still undiscovered");

  // Re-driving same segment emits nothing.
  auto ev1b = rd.update(0, 50);
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

  // Drive on Canyon Trail -> 2/2.
  auto ev2 = rd2.update(300, 40);
  check(ev2.size() == 1 && ev2[0].road_name == "Canyon Trail" && ev2[0].discovered_count == 2,
        "driving on second road brings count to 2/2");

  // SettingsPage treasure map toggle.
  carview::SettingsPage sp;
  sp.load({"--treasure-map"});
  check(sp.value("treasure map") == "on", "SettingsPage reads --treasure-map as on");
  check(sp.live_settings().treasure_map, "LiveSettings reports treasure_map=true");
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
  test_colorado_corpus_optional();
  if (g_failed == 0) std::printf("road_discovery_tests: PASS\n");
  return g_failed ? 1 : 0;
}
