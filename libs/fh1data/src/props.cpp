#include "fh1data/props.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace fh1 {
namespace {

bool same_record(const PvszRecord& a, const PvszRecord& b) {
  auto same_floats = [](const auto& x, const auto& y) {
    // Bitwise, so NaN distances compare equal to themselves.
    return std::memcmp(x.data(), y.data(), sizeof(x[0]) * x.size()) == 0;
  };
  return same_floats(a.distances, b.distances) && same_floats(a.position, b.position) &&
         same_floats(a.rotation, b.rotation) && a.unknown == b.unknown &&
         a.extended == b.extended && a.word == b.word && a.extra == b.extra && a.name == b.name;
}

}  // namespace

bool load_props(MemberSource& source, PropWorld& out, std::string* error,
                const PropLoadOptions& options) {
  out = PropWorld{};
  std::vector<uint8_t> bytes;
  for (const auto& name : source.with_suffix(".pgeo")) {
    ++out.pgeo_tiles;
    std::string why;
    if (!source.read(name, bytes, &why)) {
      ++out.pgeo_failed;
      out.failures.push_back(why);
      continue;
    }
    const uint32_t kind = pgeo_kind(bytes, &why);
    ++out.pgeo_kinds[kind];
    if (kind != 7) continue;
    PgeoTile tile;
    if (!parse_pgeo(bytes, tile, &why)) {
      ++out.pgeo_failed;
      out.failures.push_back(name + ": " + why);
      continue;
    }
    ++out.pgeo_parsed;
    ++out.pgeo_header_sizes[tile.header_size];
    out.pgeo_models += tile.models.size();
    out.pgeo_groups += tile.groups.size();
    for (const auto& m : tile.models) {
      std::vector<uint32_t> all;
      for (const auto& list : m.ids) all.insert(all.end(), list.begin(), list.end());
      bool consecutive = true;
      for (size_t k = 1; k < all.size(); ++k) consecutive &= all[k] == all[k - 1] + 1;
      out.pgeo_consecutive_records += consecutive;
      out.pgeo_records_without_list0 += m.ids[0].empty();
    }
    uint64_t packed = 0;
    for (const auto& g : tile.groups) packed += uint64_t(g.packed_count) * 40;
    out.pgeo_trailing_matches += packed == tile.trailing_size;
    const uint32_t container = uint32_t(out.containers.size());
    out.containers.push_back(name);
    for (const auto& b : tile.billboards) {
      const uint32_t key = tile.groups[b.group].key;
      if (key >= tile.references.size()) {
        ++out.tree_unresolved;
        continue;
      }
      TreeSprite t;
      // The tile's box minimum (game frame, as the instances) plus the
      // point; the axis is in the same frame.
      for (int k = 0; k < 3; ++k) t.base[size_t(k)] = tile.box_min[size_t(k)] + b.base[size_t(k)];
      t.axis = b.axis;
      t.u0 = b.u0, t.v0 = b.v0, t.u1 = b.u1, t.v1 = b.v1;
      t.half_width = b.half_width, t.height = b.height, t.near = b.near, t.far = b.far;
      t.tint = b.tint;
      t.atlas = tile.references[key].first;
      out.trees.push_back(t);
    }
    for (const auto& g : tile.groups) {
      const auto& record = tile.models[g.model_record];
      for (const auto& in : g.instances) {
        PropPlacement p;
        p.store = PropStore::kPgeo;
        p.has_object = !record.ids[0].empty();
        p.object_id = p.has_object ? record.ids[0][0] : 0;
        p.container = container;
        p.placement = pgeo_placement(in);
        out.placements.push_back(p);
        ++out.pgeo_instances;
      }
    }
  }

  std::unordered_map<uint32_t, PvszRecord> seen, seen_first;
  for (const auto& name : source.with_suffix(".pvsz")) {
    ++out.pvsz_zones;
    std::string why;
    PvszZone zone;
    if (!source.read(name, bytes, &why) || !parse_pvsz(bytes, zone, &why)) {
      ++out.pvsz_failed;
      out.failures.push_back(name + ": " + why);
      continue;
    }
    ++out.pvsz_parsed;
    out.pvsz_records += zone.records.size();
    out.pvsz_multi_start += zone.starts.size() > 1;
    const bool not_first = zone.start != zone.starts.front();
    out.pvsz_not_first_start += not_first;
    PvszZone first_zone;
    if (options.census_first_rule && not_first)
      parse_pvsz(bytes, first_zone, nullptr, PvszStartRule::kFirst);
    const uint32_t container = uint32_t(out.containers.size());
    out.containers.push_back(name);
    for (size_t k = 0; k < zone.ids.size(); ++k) {
      const uint32_t id = pvsz_object_id(zone.ids[k]);
      out.pvsz_flagged_ids += zone.ids[k] >> 31;
      out.pvsz_max_object = std::max<uint64_t>(out.pvsz_max_object, id);
      const PvszRecord& rec = zone.records[k];
      if (options.census_first_rule) {
        const PvszRecord& first = not_first ? first_zone.records[k] : rec;
        const auto [it, added] = seen_first.emplace(id, first);
        if (!added && !same_record(it->second, first)) ++out.pvsz_first_conflicts;
      }
      const auto [it, added] = seen.emplace(id, rec);
      if (!added) {
        if (!same_record(it->second, rec)) ++out.pvsz_conflicts;
        continue;
      }
      PropPlacement p;
      p.store = PropStore::kPvsz;
      p.object_id = id;
      p.container = container;
      p.placement = pvsz_placement(rec);
      out.placements.push_back(p);
    }
  }
  if (out.pgeo_tiles == 0 && out.pvsz_zones == 0) {
    if (error) *error = source.path().string() + ": no .pgeo or .pvsz members";
    return false;
  }
  return true;
}

std::string PropModels::member_name(uint32_t member) {
  char name[40];
  std::snprintf(name, sizeof name, "coloradoout.%05u.rmb.bin", member);
  return name;
}

const RenderModel* PropModels::model(uint32_t member, std::string* error) {
  if (const auto it = models_.find(member); it != models_.end()) {
    if (!it->second && error) *error = errors_[member];
    return it->second.get();
  }
  auto& slot = models_[member];
  const std::string name = member_name(member);
  std::vector<uint8_t> bytes;
  std::string why;
  if (!source_.contains(name)) {
    why = name + ": no such member";
  } else if (source_.read(name, bytes, &why)) {
    auto m = std::make_unique<RenderModel>();
    // The materials are optional: a model without them still draws flat.
    if (parse_render_model(bytes, *m, &why)) {
      std::string ignored;
      parse_render_materials(bytes, *m, &ignored);
      slot = std::move(m);
    }
    else why = name + ": " + why;
  }
  if (!slot) {
    errors_[member] = why;
    if (error) *error = why;
  }
  return slot.get();
}

bool PropModels::first_name(uint32_t member, std::string& family, int& level) {
  const RenderModel* m = model(member);
  if (!m || m->objects.empty()) return false;
  rmb_lod_part(m->objects.front().name, family, level);
  return true;
}

std::vector<uint32_t> PropModels::lod_set(uint32_t member) {
  std::string family;
  int level;
  if (!first_name(member, family, level)) return {};
  if (level < 0) return {member};
  // Down to LOD00, then up while the next member continues the count.
  uint32_t low = member;
  for (int l = level; l > 0 && low > 0; --l) {
    std::string f;
    int k;
    if (!first_name(low - 1, f, k) || f != family || k != l - 1) break;
    --low;
  }
  std::string low_family;
  int low_level;
  first_name(low, low_family, low_level);
  std::vector<uint32_t> set{low};
  for (int l = low_level + 1;; ++l) {
    std::string f;
    int k;
    const uint32_t next = set.back() + 1;
    if (!first_name(next, f, k) || f != family || k != l) break;
    set.push_back(next);
  }
  return set;
}

int64_t PropModels::lod_member(uint32_t member, int level) {
  const auto set = lod_set(member);
  if (set.empty()) return -1;
  return set[std::min<size_t>(size_t(std::max(level, 0)), set.size() - 1)];
}

}  // namespace fh1
