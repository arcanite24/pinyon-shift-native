// Roads Discovered (X / 216 in Colorado): tracks driven nodes of colorado.nav
// matching ForzaProfile's CRoadDiscovery bitset (uint32 node_count followed by
// ceil(node_count / 32) words, bit (node & 31) in word[node >> 5]). Named road
// groups are the unique `display_name` values across drawn roads (road_type in
// {dirt, b, a, freeway} -> exactly 216 in Colorado).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "traffic/nav.h"

namespace freeroam {

struct RoadDiscoveredEvent {
  std::string road_name;
  int discovered_count = 0;
  int total_count = 0;
};

class RoadDiscovery {
 public:
  // Retail GlobalRegistry UnvisitedDistanceThreshold: a road completes once the
  // total length of its unvisited segments is <= 75.0 m (and >= 1 segment visited).
  static constexpr double kUnvisitedDistanceThreshold = 75.0;

  // Indexes drawn roads and nodes from `nav`.
  bool build(const traffic::NavMap& nav);
  bool ready() const { return node_count_ > 0 && !road_names_.empty(); }

  uint32_t node_count() const { return node_count_; }
  int total_roads() const { return int(road_names_.size()); }
  int discovered_count() const { return discovered_roads_; }
  size_t visited_node_count() const { return visited_node_count_; }
  size_t visited_segment_count() const { return visited_segment_count_; }

  bool is_node_discovered(uint32_t node) const;
  bool is_segment_discovered(uint32_t a, uint32_t b) const {
    return is_node_discovered(a) && is_node_discovered(b);
  }
  bool is_road_discovered(int road_group) const;
  const std::vector<uint32_t>& words() const { return bits_; }
  const std::vector<std::string>& road_names() const { return road_names_; }

  // Restores bitset from DriveSave or ForzaProfile CRoadDiscovery words.
  void load_words(uint32_t node_count, const std::vector<uint32_t>& words);
  void reset();

  // Marks drawn road nodes within `radius_m` of (car_x, car_z) as driven
  // (without prematurely marking far segment endpoints) and returns any
  // newly discovered named roads.
  std::vector<RoadDiscoveredEvent> update(double car_x, double car_z, double radius_m = 28.0);

  // Encodes/decodes word bitset as compact lowercase hex for DriveSave.
  static std::string encode_hex(const std::vector<uint32_t>& words);
  static bool decode_hex(std::string_view hex, std::vector<uint32_t>& out);

 private:
  struct DrawnSegment {
    uint32_t a = 0, b = 0;
    double ax = 0, az = 0, bx = 0, bz = 0;
    double length = 0.0;
    int road_group = -1;
  };

  bool is_road_complete(int road_group) const;
  bool mark_node(uint32_t node, std::vector<RoadDiscoveredEvent>& events);
  void recompute_discovered_roads();

  uint32_t node_count_ = 0;
  std::vector<uint32_t> bits_;
  std::vector<std::string> road_names_;
  std::vector<std::vector<uint32_t>> road_nodes_;  // road_group -> node indices
  std::vector<std::vector<uint32_t>> road_segs_;   // road_group -> segment indices in segs_
  std::vector<std::vector<int>> node_roads_;       // node -> incident road_group(s)
  std::vector<std::vector<uint32_t>> node_segs_;   // node -> incident segment indices in segs_
  std::vector<uint8_t> road_discovered_;
  int discovered_roads_ = 0;
  size_t visited_node_count_ = 0;
  size_t visited_segment_count_ = 0;

  // Spatial grid over drawn road segments for fast per-frame updates.
  std::vector<DrawnSegment> segs_;
  double cell_ = 100.0, min_x_ = 0.0, min_z_ = 0.0;
  int nx_ = 0, nz_ = 0;
  std::vector<std::vector<uint32_t>> grid_;
};

}  // namespace freeroam
