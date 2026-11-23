#include "fh1data/props.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
  const auto pgeo_names = source.with_suffix(".pgeo");
  struct PgeoResult {
    bool read_ok = false;
    uint32_t kind = 0;
    bool parsed_ok = false;
    std::string why;
    PgeoTile tile;
  };
  std::vector<PgeoResult> pgeo_res(pgeo_names.size());
  {
    const size_t n = pgeo_names.size();
    const size_t threads = std::min<size_t>(n, std::max<size_t>(1, std::thread::hardware_concurrency()));
    if (threads > 1 && n >= 4) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          std::vector<uint8_t> bytes;
          for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) break;
            auto& r = pgeo_res[i];
            if (!source.read(pgeo_names[i], bytes, &r.why)) continue;
            r.read_ok = true;
            r.kind = pgeo_kind(bytes, &r.why);
            if (r.kind != 7) continue;
            r.parsed_ok = parse_pgeo(bytes, r.tile, &r.why);
          }
        });
      }
      for (auto& w : workers) w.join();
    } else {
      std::vector<uint8_t> bytes;
      for (size_t i = 0; i < n; ++i) {
        auto& r = pgeo_res[i];
        if (!source.read(pgeo_names[i], bytes, &r.why)) continue;
        r.read_ok = true;
        r.kind = pgeo_kind(bytes, &r.why);
        if (r.kind != 7) continue;
        r.parsed_ok = parse_pgeo(bytes, r.tile, &r.why);
      }
    }
  }
  for (size_t i = 0; i < pgeo_names.size(); ++i) {
    const auto& name = pgeo_names[i];
    auto& r = pgeo_res[i];
    ++out.pgeo_tiles;
    if (!r.read_ok) {
      ++out.pgeo_failed;
      out.failures.push_back(r.why);
      continue;
    }
    ++out.pgeo_kinds[r.kind];
    if (r.kind != 7) continue;
    if (!r.parsed_ok) {
      ++out.pgeo_failed;
      out.failures.push_back(name + ": " + r.why);
      continue;
    }
    const PgeoTile& tile = r.tile;
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
      for (int k = 0; k < 3; ++k) t.base[size_t(k)] = tile.box_min[size_t(k)] + b.base[size_t(k)];
      t.axis = b.axis;
      t.u0 = b.u0, t.v0 = b.v0, t.u1 = b.u1, t.v1 = b.v1;
      t.half_width = b.half_width, t.height = b.height, t.near = b.near, t.far = b.far;
      t.tint = b.tint;
      t.atlas = tile.references[key].first;
      t.routes = tile.routes;
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
        p.routes = tile.routes;
        out.placements.push_back(p);
        ++out.pgeo_instances;
      }
    }
  }
  pgeo_res.clear();

  const auto pvsz_names = source.with_suffix(".pvsz");
  struct PvszResult {
    bool ok = false;
    std::string why;
    PvszZone zone;
    PvszZone first_zone;
    bool has_first = false;
  };
  std::vector<PvszResult> pvsz_res(pvsz_names.size());
  {
    const size_t n = pvsz_names.size();
    const size_t threads = std::min<size_t>(n, std::max<size_t>(1, std::thread::hardware_concurrency()));
    if (threads > 1 && n >= 4) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          std::vector<uint8_t> bytes;
          for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) break;
            auto& r = pvsz_res[i];
            if (!source.read(pvsz_names[i], bytes, &r.why) || !parse_pvsz(bytes, r.zone, &r.why)) continue;
            r.ok = true;
            if (options.census_first_rule && !r.zone.starts.empty() && r.zone.start != r.zone.starts.front()) {
              r.has_first = true;
              parse_pvsz(bytes, r.first_zone, nullptr, PvszStartRule::kFirst);
            }
          }
        });
      }
      for (auto& w : workers) w.join();
    } else {
      std::vector<uint8_t> bytes;
      for (size_t i = 0; i < n; ++i) {
        auto& r = pvsz_res[i];
        if (!source.read(pvsz_names[i], bytes, &r.why) || !parse_pvsz(bytes, r.zone, &r.why)) continue;
        r.ok = true;
        if (options.census_first_rule && !r.zone.starts.empty() && r.zone.start != r.zone.starts.front()) {
          r.has_first = true;
          parse_pvsz(bytes, r.first_zone, nullptr, PvszStartRule::kFirst);
        }
      }
    }
  }
  std::unordered_map<uint32_t, PvszRecord> seen, seen_first;
  for (size_t i = 0; i < pvsz_names.size(); ++i) {
    const auto& name = pvsz_names[i];
    auto& r = pvsz_res[i];
    ++out.pvsz_zones;
    if (!r.ok) {
      ++out.pvsz_failed;
      out.failures.push_back(name + ": " + r.why);
      continue;
    }
    const PvszZone& zone = r.zone;
    ++out.pvsz_parsed;
    out.pvsz_records += zone.records.size();
    out.pvsz_multi_start += zone.starts.size() > 1;
    const bool not_first = !zone.starts.empty() && zone.start != zone.starts.front();
    out.pvsz_not_first_start += not_first;
    const uint32_t container = uint32_t(out.containers.size());
    out.containers.push_back(name);
    for (size_t k = 0; k < zone.ids.size(); ++k) {
      const uint32_t id = pvsz_object_id(zone.ids[k]);
      out.pvsz_flagged_ids += zone.ids[k] >> 31;
      out.pvsz_max_object = std::max<uint64_t>(out.pvsz_max_object, id);
      const PvszRecord& rec = zone.records[k];
      if (options.census_first_rule) {
        const PvszRecord& first = r.has_first ? r.first_zone.records[k] : rec;
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
      if (rec.extended) p.routes = rec.extra;
      for (char ch : rec.name) p.name += char(std::tolower(static_cast<unsigned char>(ch)));
      out.placements.push_back(p);
    }
  }
  if (out.pgeo_tiles == 0 && out.pvsz_zones == 0) {
    if (error) *error = source.path().string() + ": no .pgeo or .pvsz members";
    return false;
  }
  return true;
}

std::string PropModels::member_name(uint32_t member, const std::string& prefix) {
  char number[16];
  std::snprintf(number, sizeof number, ".%05u.rmb.bin", member);
  return prefix + number;
}

const RenderModel* PropModels::model(uint32_t member, std::string* error) {
  if (const auto it = models_.find(member); it != models_.end()) {
    if (!it->second && error) *error = errors_[member];
    return it->second.get();
  }
  auto& slot = models_[member];
  const std::string name = name_of(member);
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

void PropModels::preload(const std::vector<uint32_t>& members) {
  std::vector<uint32_t> todo;
  todo.reserve(members.size());
  std::unordered_set<uint32_t> seen;
  for (uint32_t m : members) {
    if (models_.count(m) || !seen.insert(m).second) continue;
    todo.push_back(m);
  }
  if (todo.size() < 4) return;
  const size_t threads = std::min<size_t>(todo.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
  if (threads <= 1) return;
  struct LoadedModel {
    std::unique_ptr<RenderModel> model;
    std::string why;
  };
  std::vector<LoadedModel> results(todo.size());
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&]() {
      std::vector<uint8_t> bytes;
      for (;;) {
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= todo.size()) break;
        const std::string name = name_of(todo[i]);
        auto& r = results[i];
        if (!source_.contains(name)) {
          r.why = name + ": no such member";
          continue;
        }
        if (source_.read(name, bytes, &r.why)) {
          auto m = std::make_unique<RenderModel>();
          if (parse_render_model(bytes, *m, &r.why)) {
            std::string ignored;
            parse_render_materials(bytes, *m, &ignored);
            r.model = std::move(m);
          } else {
            r.why = name + ": " + r.why;
          }
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  for (size_t i = 0; i < todo.size(); ++i) {
    const uint32_t m = todo[i];
    if (results[i].model) {
      models_[m] = std::move(results[i].model);
    } else {
      models_[m] = nullptr;
      errors_[m] = std::move(results[i].why);
    }
  }
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
