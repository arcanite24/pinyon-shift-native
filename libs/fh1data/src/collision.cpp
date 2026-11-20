#include "fh1data/collision.h"

#include <cstdlib>

#include "fh1data/bytes.h"

namespace fh1 {

namespace {

bool fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

}  // namespace

bool parse_fiz(std::span<const uint8_t> data, FizTile& tile, std::string* error) {
  tile = FizTile{};
  try {
    BeReader h(data);
    const auto magic = h.raw(4);
    if (magic[0] != 'f' || magic[1] != 'i' || magic[2] != 'z' || magic[3] != ' ') return fail(error, "not a fiz tile");
    tile.version = h.u32();
    tile.block_a_size = h.u32();
    tile.block_b_size = h.u32();
    tile.block_b_records = h.u32();
    if (tile.version != 1) return fail(error, "fiz version " + std::to_string(tile.version));
    constexpr size_t kHeader = 32;
    if (kHeader + size_t(tile.block_a_size) + tile.block_b_size != data.size())
      return fail(error, "fiz block sizes do not add up to the file size");
    const auto a = data.subspan(kHeader, tile.block_a_size);
    BeReader r(a);
    const uint32_t vertex_count = r.u32();
    const uint32_t triangle_count = r.u32();
    const uint32_t vertex_offset = r.u32();
    const uint32_t triangle_offset = r.u32();
    tile.grid_x = r.u32();
    tile.grid_z = r.u32();
    for (auto& b : tile.box) b = r.f32();
    tile.cell[0] = r.f32();
    tile.cell[1] = r.f32();
    r.skip(8);  // the cell size's inverses
    const uint32_t grid_offset = r.u32();
    r.seek(122);
    const uint16_t route_count = r.u16();
    if (route_count > 15) return fail(error, "fiz route list longer than the mask");
    r.seek(60);
    tile.routes.resize(route_count);
    for (auto& route : tile.routes) route = r.u16();

    tile.vertices.resize(vertex_count);
    r.seek(vertex_offset);
    for (auto& v : tile.vertices) {
      v = {r.f32(), r.f32(), r.f32()};
      r.skip(4);  // not decoded
    }
    tile.triangles.resize(triangle_count);
    r.seek(triangle_offset);
    for (auto& t : tile.triangles) {
      t.flags = r.u8();
      t.surface = r.u8();
      t.word = r.u16();
      for (auto& v : t.v) {
        const uint32_t at = r.u32();
        if (at < vertex_offset || (at - vertex_offset) % 16 != 0 || (at - vertex_offset) / 16 >= vertex_count)
          return fail(error, "fiz triangle vertex reference out of range");
        v = (at - vertex_offset) / 16;
      }
      for (auto& n : t.normal) n = r.f32();
      t.edges = r.u32();
    }
    if (tile.grid_x > 256 || tile.grid_z > 256) return fail(error, "fiz grid too large");
    tile.grid.resize(size_t(tile.grid_x) * tile.grid_z);
    for (size_t c = 0; c < tile.grid.size(); ++c) {
      r.seek(grid_offset + 4 * c);
      const uint32_t at = r.u32();
      if (at == 0) continue;
      BeReader cell(a, at);
      auto& list = tile.grid[c];
      list.resize(cell.u16());
      for (auto& i : list) {
        i = cell.u16();
        if (i >= triangle_count) return fail(error, "fiz grid lists a triangle out of range");
      }
    }
  } catch (const FormatError& e) {
    return fail(error, std::string("fiz: ") + e.what());
  }
  return true;
}

bool parse_col(std::span<const uint8_t> data, ColIndex& index, std::string* error) {
  index = ColIndex{};
  try {
    BeReader r(data);
    index.width = r.u32();
    index.height = r.u32();
    index.x0 = r.f32();
    index.z0 = r.f32();
    r.seek(76);
    const uint32_t count = r.u32();
    const uint32_t table = r.u32();
    r.seek(table);
    index.records.resize(count);
    for (auto& rec : index.records) {
      rec.stream_offset = r.u32();
      rec.tile = r.u32();
      rec.cell = r.u32();
      r.skip(4);  // the stream offset again
      rec.block_size = r.u32();
      r.skip(4);
      if (rec.cell >= index.width * index.height) return fail(error, "col record cell out of range");
    }
  } catch (const FormatError& e) {
    return fail(error, std::string("col: ") + e.what());
  }
  return true;
}

namespace {

// The value of attribute `name` in the tag starting at `at` (up to '>').
bool attribute(std::string_view xml, size_t at, std::string_view name, std::string& out) {
  const size_t end = xml.find('>', at);
  if (end == std::string_view::npos) return false;
  const std::string_view tag = xml.substr(at, end - at);
  for (size_t p = tag.find(name); p != std::string_view::npos; p = tag.find(name, p + 1)) {
    if (p == 0 || (tag[p - 1] != ' ' && tag[p - 1] != '\t')) continue;
    size_t q = p + name.size();
    while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
    if (q >= tag.size() || tag[q] != '=') continue;
    ++q;
    while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
    if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
    const size_t close = tag.find(tag[q], q + 1);
    if (close == std::string_view::npos) return false;
    out = std::string(tag.substr(q + 1, close - q - 1));
    return true;
  }
  return false;
}

// The position of the element `<name` (followed by space, '/' or '>') in
// [from, to), or npos.
size_t find_element(std::string_view xml, std::string_view name, size_t from, size_t to) {
  const std::string open = "<" + std::string(name);
  for (size_t p = xml.find(open, from); p != std::string_view::npos && p < to; p = xml.find(open, p + 1)) {
    const size_t q = p + open.size();
    if (q < xml.size() && (xml[q] == ' ' || xml[q] == '\t' || xml[q] == '/' || xml[q] == '>' || xml[q] == '\r' ||
                           xml[q] == '\n'))
      return p;
  }
  return std::string_view::npos;
}

}  // namespace

bool parse_surface_types(std::string_view xml, std::vector<SurfaceType>& out, std::string* error) {
  out.clear();
  const size_t list = find_element(xml, "SurfaceTypesList", 0, xml.size());
  if (list == std::string_view::npos) return fail(error, "no SurfaceTypesList");
  const size_t list_end = xml.find("</SurfaceTypesList>", list);
  if (list_end == std::string_view::npos) return fail(error, "SurfaceTypesList not closed");
  for (size_t p = find_element(xml, "SurfaceType", list + 1, list_end); p != std::string_view::npos;
       p = find_element(xml, "SurfaceType", p + 1, list_end)) {
    SurfaceType s;
    if (!attribute(xml, p, "value", s.name)) return fail(error, "SurfaceType without a value");
    out.push_back(s);
  }
  if (out.empty()) return fail(error, "empty SurfaceTypesList");

  const size_t defs = find_element(xml, "SurfaceTypes", list_end, xml.size());
  if (defs == std::string_view::npos) return fail(error, "no SurfaceTypes");
  const size_t defs_end = xml.find("</SurfaceTypes>", defs);
  if (defs_end == std::string_view::npos) return fail(error, "SurfaceTypes not closed");
  auto read_def = [&](SurfaceType& s) {
    const size_t at = find_element(xml, s.name, defs + 1, defs_end);
    if (at == std::string_view::npos) return;
    const size_t end = xml.find("</" + s.name + ">", at);
    if (end == std::string_view::npos || end > defs_end) return;
    s.defined = true;
    attribute(xml, at, "Category", s.category);
    auto read = [&](const char* element, float& out) {
      const size_t f = find_element(xml, element, at, end);
      std::string v;
      if (f != std::string_view::npos && attribute(xml, f, "value", v)) out = float(std::atof(v.c_str()));
    };
    read("FrictionScale", s.friction_scale);
    read("RearGripMultiplier", s.rear_grip);
    read("OffRoadness", s.off_roadness);
    read("OffRoadDryPeakSA", s.off_road_peak_sa);
  };
  for (auto& s : out) read_def(s);
  // An index without a definition takes Asphalt's values (index 0).
  for (auto& s : out) {
    if (s.defined) continue;
    s.category = out[0].category;
    s.friction_scale = out[0].friction_scale;
    s.rear_grip = out[0].rear_grip;
    s.off_roadness = out[0].off_roadness;
    s.off_road_peak_sa = out[0].off_road_peak_sa;
  }
  return true;
}

}  // namespace fh1
