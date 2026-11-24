#include "freeroam/road_discovery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace freeroam {
namespace {

bool is_drawn_road_type(std::string_view t) {
  return t == "dirt" || t == "b" || t == "a" || t == "freeway";
}

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

}  // namespace

bool RoadDiscovery::build(const traffic::NavMap& nav) {
  node_count_ = uint32_t(nav.nodes.size());
  bits_.assign((size_t(node_count_) + 31u) / 32u, 0u);
  road_names_.clear();
  road_nodes_.clear();
  road_segs_.clear();
  node_roads_.assign(node_count_, {});
  node_segs_.assign(node_count_, {});
  segs_.clear();
  grid_.clear();
  discovered_roads_ = 0;
  visited_node_count_ = 0;
  visited_segment_count_ = 0;

  std::map<std::string, int> name_to_group;
  double max_x = -1e30, max_z = -1e30;
  min_x_ = min_z_ = 1e30;

  for (uint32_t w = 0; w < uint32_t(nav.ways.size()); ++w) {
    const std::string_view rt = nav.way_tag(w, "road_type");
    if (!is_drawn_road_type(rt) || nav.ways[w].node_count < 2) continue;

    std::string dn(nav.way_tag(w, "display_name"));
    if (dn.empty()) dn = "way_" + std::to_string(w);
    auto [it, inserted] = name_to_group.emplace(dn, int(road_names_.size()));
    const int grp = it->second;
    if (inserted) {
      road_names_.push_back(dn);
      road_nodes_.emplace_back();
      road_segs_.emplace_back();
    }

    const uint32_t nc = nav.ways[w].node_count;
    for (uint32_t k = 0; k < nc; ++k) {
      const uint32_t n = nav.way_node(w, k);
      if (n >= node_count_) continue;
      road_nodes_[size_t(grp)].push_back(n);
      auto& nr = node_roads_[n];
      if (std::find(nr.begin(), nr.end(), grp) == nr.end()) nr.push_back(grp);
      const auto& nd = nav.nodes[n];
      min_x_ = std::min(min_x_, double(nd.x));
      max_x = std::max(max_x, double(nd.x));
      min_z_ = std::min(min_z_, double(nd.z));
      max_z = std::max(max_z, double(nd.z));
    }
    for (uint32_t k = 0; k + 1 < nc; ++k) {
      const uint32_t a = nav.way_node(w, k), b = nav.way_node(w, k + 1);
      if (a >= node_count_ || b >= node_count_ || a == b) continue;
      const auto& na = nav.nodes[a];
      const auto& nb = nav.nodes[b];
      const double len = std::hypot(double(nb.x) - double(na.x), double(nb.z) - double(na.z));
      const uint32_t si = uint32_t(segs_.size());
      segs_.push_back({a, b, na.x, na.z, nb.x, nb.z, len, grp});
      road_segs_[size_t(grp)].push_back(si);
      node_segs_[a].push_back(si);
      node_segs_[b].push_back(si);
    }
  }

  for (auto& rn : road_nodes_) {
    std::sort(rn.begin(), rn.end());
    rn.erase(std::unique(rn.begin(), rn.end()), rn.end());
  }
  road_discovered_.assign(road_names_.size(), 0);

  if (segs_.empty() || road_names_.empty()) return false;

  nx_ = int((max_x - min_x_) / cell_) + 1;
  nz_ = int((max_z - min_z_) / cell_) + 1;
  grid_.assign(size_t(nx_) * size_t(nz_), {});
  for (uint32_t si = 0; si < uint32_t(segs_.size()); ++si) {
    const auto& s = segs_[si];
    const int x0 = std::clamp(int((std::min(s.ax, s.bx) - min_x_) / cell_), 0, nx_ - 1);
    const int x1 = std::clamp(int((std::max(s.ax, s.bx) - min_x_) / cell_), 0, nx_ - 1);
    const int z0 = std::clamp(int((std::min(s.az, s.bz) - min_z_) / cell_), 0, nz_ - 1);
    const int z1 = std::clamp(int((std::max(s.az, s.bz) - min_z_) / cell_), 0, nz_ - 1);
    for (int gz = z0; gz <= z1; ++gz)
      for (int gx = x0; gx <= x1; ++gx) grid_[size_t(gz) * size_t(nx_) + size_t(gx)].push_back(si);
  }
  return true;
}

bool RoadDiscovery::is_node_discovered(uint32_t node) const {
  if (node >= node_count_) return false;
  const size_t wi = size_t(node >> 5);
  return wi < bits_.size() && ((bits_[wi] >> (node & 31u)) & 1u) != 0;
}

bool RoadDiscovery::is_road_discovered(int road_group) const {
  return road_group >= 0 && size_t(road_group) < road_discovered_.size() && road_discovered_[size_t(road_group)] != 0;
}

void RoadDiscovery::reset() {
  std::fill(bits_.begin(), bits_.end(), 0u);
  std::fill(road_discovered_.begin(), road_discovered_.end(), uint8_t{0});
  discovered_roads_ = 0;
  visited_node_count_ = 0;
  visited_segment_count_ = 0;
}

bool RoadDiscovery::is_road_complete(int road_group) const {
  if (road_group < 0 || size_t(road_group) >= road_segs_.size()) return false;
  const auto& rsegs = road_segs_[size_t(road_group)];
  if (rsegs.empty()) {
    const auto& rn = road_nodes_[size_t(road_group)];
    if (rn.empty()) return false;
    for (uint32_t n : rn) {
      if (!is_node_discovered(n)) return false;
    }
    return true;
  }
  double unvisited_len = 0.0;
  bool any_visited = false;
  for (uint32_t si : rsegs) {
    const auto& s = segs_[si];
    if (is_node_discovered(s.a) && is_node_discovered(s.b)) {
      any_visited = true;
    } else {
      unvisited_len += s.length;
    }
  }
  return any_visited && unvisited_len <= kUnvisitedDistanceThreshold;
}

void RoadDiscovery::recompute_discovered_roads() {
  discovered_roads_ = 0;
  visited_node_count_ = 0;
  visited_segment_count_ = 0;
  std::fill(road_discovered_.begin(), road_discovered_.end(), uint8_t{0});
  for (uint32_t n = 0; n < node_count_; ++n) {
    if (is_node_discovered(n)) ++visited_node_count_;
  }
  for (const auto& s : segs_) {
    if (is_node_discovered(s.a) && is_node_discovered(s.b)) ++visited_segment_count_;
  }
  for (size_t g = 0; g < road_names_.size(); ++g) {
    if (is_road_complete(int(g))) {
      road_discovered_[g] = 1;
      ++discovered_roads_;
    }
  }
}

void RoadDiscovery::load_words(uint32_t node_count, const std::vector<uint32_t>& words) {
  if (!ready()) return;
  reset();
  const size_t copy_words = std::min(bits_.size(), words.size());
  for (size_t i = 0; i < copy_words; ++i) bits_[i] = words[i];
  // Mask out any bits past min(node_count_, node_count).
  const uint32_t valid_nodes = std::min(node_count_, node_count > 0 ? node_count : node_count_);
  if (valid_nodes < node_count_) {
    const size_t first_word = size_t(valid_nodes >> 5);
    const uint32_t rem = valid_nodes & 31u;
    if (first_word < bits_.size()) {
      bits_[first_word] &= (rem == 0 ? 0u : ((1u << rem) - 1u));
      for (size_t i = first_word + 1; i < bits_.size(); ++i) bits_[i] = 0u;
    }
  }
  recompute_discovered_roads();
}

bool RoadDiscovery::mark_node(uint32_t node, std::vector<RoadDiscoveredEvent>& events) {
  if (node >= node_count_) return false;
  const size_t wi = size_t(node >> 5);
  const uint32_t mask = 1u << (node & 31u);
  if ((bits_[wi] & mask) != 0) return false;
  bits_[wi] |= mask;
  ++visited_node_count_;
  for (uint32_t si : node_segs_[node]) {
    const auto& s = segs_[si];
    const uint32_t other = (s.a == node) ? s.b : s.a;
    if (is_node_discovered(other)) ++visited_segment_count_;
  }
  for (int grp : node_roads_[node]) {
    if (grp >= 0 && size_t(grp) < road_discovered_.size() && !road_discovered_[size_t(grp)]) {
      if (is_road_complete(grp)) {
        road_discovered_[size_t(grp)] = 1;
        ++discovered_roads_;
        events.push_back({road_names_[size_t(grp)], discovered_roads_, int(road_names_.size())});
      }
    }
  }
  return true;
}

std::vector<RoadDiscoveredEvent> RoadDiscovery::update(double car_x, double car_z, double radius_m) {
  std::vector<RoadDiscoveredEvent> events;
  if (!ready() || grid_.empty()) return events;
  const int x0 = std::max(0, int((car_x - radius_m - min_x_) / cell_));
  const int x1 = std::min(nx_ - 1, int((car_x + radius_m - min_x_) / cell_));
  const int z0 = std::max(0, int((car_z - radius_m - min_z_) / cell_));
  const int z1 = std::min(nz_ - 1, int((car_z + radius_m - min_z_) / cell_));
  if (x0 > x1 || z0 > z1) return events;

  const double r2 = radius_m * radius_m;
  for (int gz = z0; gz <= z1; ++gz) {
    for (int gx = x0; gx <= x1; ++gx) {
      for (uint32_t si : grid_[size_t(gz) * size_t(nx_) + size_t(gx)]) {
        const auto& s = segs_[si];
        // Mark only individual endpoints actually within driving proximity `radius_m`
        // (never blindly mark a far endpoint 79 m away just because the segment passes near).
        const double da2 = (car_x - s.ax) * (car_x - s.ax) + (car_z - s.az) * (car_z - s.az);
        if (da2 <= r2) mark_node(s.a, events);
        const double db2 = (car_x - s.bx) * (car_x - s.bx) + (car_z - s.bz) * (car_z - s.bz);
        if (db2 <= r2) mark_node(s.b, events);
      }
    }
  }
  return events;
}

std::string RoadDiscovery::encode_hex(const std::vector<uint32_t>& words) {
  // Trim trailing zero words so fresh/early saves stay compact.
  size_t last = words.size();
  while (last > 0 && words[last - 1] == 0u) --last;
  if (last == 0) return "0";
  std::string out;
  out.reserve(last * 8);
  char buf[9];
  for (size_t i = 0; i < last; ++i) {
    std::snprintf(buf, sizeof buf, "%08x", words[i]);
    out.append(buf, 8);
  }
  return out;
}

bool RoadDiscovery::decode_hex(std::string_view hex, std::vector<uint32_t>& out) {
  out.clear();
  if (hex.empty() || hex == "0") return true;
  if (hex.size() % 8 != 0) return false;
  const size_t n = hex.size() / 8;
  out.resize(n, 0u);
  for (size_t i = 0; i < n; ++i) {
    uint32_t v = 0;
    for (size_t k = 0; k < 8; ++k) {
      const int nib = hex_nibble(hex[i * 8 + k]);
      if (nib < 0) return false;
      v = (v << 4) | uint32_t(nib);
    }
    out[i] = v;
  }
  return true;
}

}  // namespace freeroam
