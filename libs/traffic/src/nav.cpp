#include "traffic/nav.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace traffic {
namespace {

struct Reader {
  const std::vector<uint8_t>& b;
  size_t at = 0;
  bool ok = true;
  uint32_t u32() {
    if (at + 4 > b.size()) {
      ok = false;
      return 0;
    }
    const uint32_t v = uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
    at += 4;
    return v;
  }
  float f32() {
    const uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
};

// The zero-terminated strings at the offsets, within a blob of `bytes`.
bool strings(Reader& r, uint32_t count, uint32_t bytes, const std::vector<uint32_t>& offsets,
             std::vector<std::string>& out) {
  if (r.at + bytes > r.b.size()) return false;
  const char* blob = reinterpret_cast<const char*>(r.b.data() + r.at);
  out.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (offsets[i] >= bytes) return false;
    const char* s = blob + offsets[i];
    out[i].assign(s, strnlen(s, bytes - offsets[i]));
  }
  r.at += bytes;
  return true;
}

}  // namespace

std::string_view NavMap::tag(uint32_t first, uint32_t count, std::string_view key) const {
  for (uint32_t i = first; i < first + count && i < tags.size(); ++i)
    if (tags[i].key < keys.size() && keys[tags[i].key] == key && tags[i].value < values.size())
      return values[tags[i].value];
  return {};
}

std::string_view NavMap::way_tag(uint32_t way, std::string_view key) const {
  return tag(ways[way].tag_first, ways[way].tag_count, key);
}

std::string_view NavMap::node_tag(uint32_t node, std::string_view key) const {
  return tag(nodes[node].tag_first, nodes[node].tag_count, key);
}

double NavMap::way_length(uint32_t way) const {
  double s = 0;
  for (uint32_t k = 1; k < ways[way].node_count; ++k) {
    const NavNode &a = nodes[way_node(way, k - 1)], &b = nodes[way_node(way, k)];
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    s += std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  return s;
}

bool parse_nav(const std::vector<uint8_t>& bytes, NavMap& out, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = std::string("nav: ") + what;
    return false;
  };
  Reader r{bytes};
  for (uint32_t& h : out.header) h = r.u32();
  if (!r.ok) return fail("short header");
  if (out.header[0] != NavMap::kMagic) return fail("bad magic");
  const uint32_t n = out.header[1], w = out.header[2], t = out.header[3], refs = out.header[4],
                 back = out.header[5], k = out.header[6], v = out.header[7];
  const uint64_t need = 40 + uint64_t(n) * 32 + uint64_t(w) * 16 + uint64_t(refs) * 4 + uint64_t(back) * 8 +
                        uint64_t(t) * 8 + uint64_t(k) * 4 + uint64_t(v) * 4 + out.header[8] + out.header[9] + 4;
  if (need != bytes.size()) return fail("size does not match the header");
  out.nodes.resize(n);
  for (auto& nd : out.nodes) {
    nd.id = r.u32();
    nd.x = r.f32(), nd.y = r.f32(), nd.z = r.f32();
    nd.way_ref_count = r.u32(), nd.way_ref_first = r.u32();
    nd.tag_count = r.u32(), nd.tag_first = r.u32();
  }
  out.ways.resize(w);
  for (auto& wy : out.ways) {
    wy.node_count = r.u32(), wy.node_first = r.u32();
    wy.tag_count = r.u32(), wy.tag_first = r.u32();
  }
  out.way_nodes.resize(refs);
  for (auto& x : out.way_nodes) x = r.u32();
  out.node_ways.resize(back);
  for (auto& x : out.node_ways) x.way = r.u32(), x.position = r.u32();
  out.tags.resize(t);
  for (auto& x : out.tags) x.key = r.u32(), x.value = r.u32();
  std::vector<uint32_t> key_offsets(k), value_offsets(v);
  for (auto& x : key_offsets) x = r.u32();
  for (auto& x : value_offsets) x = r.u32();
  if (!strings(r, k, out.header[8], key_offsets, out.keys) || !strings(r, v, out.header[9], value_offsets, out.values))
    return fail("bad string table");
  if (r.u32() != NavMap::kMagic || !r.ok) return fail("bad trailer");
  // References in range.
  for (const auto& wy : out.ways)
    if (uint64_t(wy.node_first) + wy.node_count > refs || uint64_t(wy.tag_first) + wy.tag_count > t)
      return fail("way reference out of range");
  for (uint32_t x : out.way_nodes)
    if (x >= n) return fail("way-node reference out of range");
  for (const auto& nd : out.nodes)
    if (uint64_t(nd.way_ref_first) + nd.way_ref_count > back || uint64_t(nd.tag_first) + nd.tag_count > t)
      return fail("node reference out of range");
  for (const auto& x : out.node_ways)
    if (x.way >= w || x.position >= out.ways[x.way].node_count) return fail("node-way reference out of range");
  for (const auto& x : out.tags)
    if (x.key >= k || x.value >= v) return fail("tag out of range");
  return true;
}

bool read_nav(const std::filesystem::path& path, NavMap& out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "nav: cannot open " + path.string();
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return parse_nav(bytes, out, error);
}

}  // namespace traffic
