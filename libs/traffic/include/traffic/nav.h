// The road network (SPEC-TRAFFIC, "The road network"): a world's `.nav`
// map, OpenStreetMap-style nodes with positions, ways listing nodes in
// order, and key-value tags on both. Big-endian 32-bit words throughout:
// ten header words, nine tables, and the magic again as the last word.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace traffic {

struct NavTag {
  uint32_t key = 0, value = 0;  // indices into NavMap::keys and values
};

struct NavNode {
  uint32_t id = 0;
  float x = 0, y = 0, z = 0;  // the physics world frame, y up, m
  uint32_t way_ref_count = 0, way_ref_first = 0;  // into NavMap::node_ways
  uint32_t tag_count = 0, tag_first = 0;          // into NavMap::tags
};

struct NavWay {
  uint32_t node_count = 0, node_first = 0;  // into NavMap::way_nodes
  uint32_t tag_count = 0, tag_first = 0;
};

struct NavNodeWay {
  uint32_t way = 0, position = 0;  // a way index and the node's place in it
};

struct NavMap {
  static constexpr uint32_t kMagic = 0x0E177551;
  uint32_t header[10] = {};
  std::vector<NavNode> nodes;
  std::vector<NavWay> ways;
  std::vector<uint32_t> way_nodes;  // node indices (not ids)
  std::vector<NavNodeWay> node_ways;
  std::vector<NavTag> tags;
  std::vector<std::string> keys, values;

  // The value of `key` among the tags [first, first + count), or empty.
  std::string_view tag(uint32_t first, uint32_t count, std::string_view key) const;
  std::string_view way_tag(uint32_t way, std::string_view key) const;
  std::string_view node_tag(uint32_t node, std::string_view key) const;
  // The node index of a way's k-th node.
  uint32_t way_node(uint32_t way, uint32_t k) const { return way_nodes[ways[way].node_first + k]; }
  double way_length(uint32_t way) const;  // m, along its segments (3-D)
};

bool parse_nav(const std::vector<uint8_t>& bytes, NavMap& out, std::string* error);
bool read_nav(const std::filesystem::path& path, NavMap& out, std::string* error);

}  // namespace traffic
