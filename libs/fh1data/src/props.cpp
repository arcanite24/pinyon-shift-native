#include "fh1data/props.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
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

std::string_view lod_prefix(const std::string& family) {
  const size_t at = family.find("_lod**");
  return at == std::string::npos ? std::string_view(family) : std::string_view(family.data(), at);
}

// Verifies two consecutive LOD candidates occupy the same bounding region and
// scale, so trailing artist suffixes after "_LODnn" unify cleanly without
// merging unrelated sequential barrier or terrain chunks.
bool lod_boxes_compatible(const RenderModel& a, const RenderModel& b) {
  float ca[3], cb[3], ea[3], eb[3];
  float max_ext = 0.0f;
  for (int i = 0; i < 3; ++i) {
    ca[i] = 0.5f * (a.box_min[i] + a.box_max[i]);
    cb[i] = 0.5f * (b.box_min[i] + b.box_max[i]);
    ea[i] = std::max(0.0f, a.box_max[i] - a.box_min[i]);
    eb[i] = std::max(0.0f, b.box_max[i] - b.box_min[i]);
    max_ext = std::max({max_ext, ea[i], eb[i]});
  }
  const float tol_c = std::max(1.5f, max_ext * 0.20f);
  const float dx = ca[0] - cb[0], dy = ca[1] - cb[1], dz = ca[2] - cb[2];
  if (dx * dx + dy * dy + dz * dz > tol_c * tol_c) return false;
  for (int i = 0; i < 3; ++i) {
    const float hi = std::max(ea[i], eb[i]);
    const float lo = std::min(ea[i], eb[i]);
    if (hi > 1.0f && lo < hi * 0.65f && (hi - lo) > 2.5f) return false;
  }
  return true;
}

}  // namespace

uint16_t PropWorld::intern_routes(const std::vector<uint8_t>& routes) {
  if (routes.empty()) return 0;
  if (route_lists.empty()) route_lists.emplace_back();
  for (size_t i = 1; i < route_lists.size(); ++i) {
    if (route_lists[i] == routes) return uint16_t(i);
  }
  if (route_lists.size() >= 65535u) return 0;
  route_lists.push_back(routes);
  return uint16_t(route_lists.size() - 1);
}

bool load_props_cache(const std::filesystem::path& path, uint64_t zip_size, PropWorld& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  auto rd = [&](void* dst, size_t n) -> bool {
    return n == 0 || bool(in.read(reinterpret_cast<char*>(dst), std::streamsize(n)));
  };
  char magic[8]{};
  uint64_t cached_zip_size = 0;
  if (!rd(magic, 8) || std::memcmp(magic, "SHPR0001", 8) != 0) return false;
  if (!rd(&cached_zip_size, 8) || cached_zip_size != zip_size) return false;

  PropWorld tmp;
  tmp.route_lists.clear();

  uint32_t n_routes = 0;
  if (!rd(&n_routes, 4) || n_routes == 0 || n_routes > 65535u) return false;
  tmp.route_lists.resize(n_routes);
  for (uint32_t i = 0; i < n_routes; ++i) {
    uint16_t len = 0;
    if (!rd(&len, 2) || len > 1024u) return false;
    tmp.route_lists[i].resize(len);
    if (!rd(tmp.route_lists[i].data(), len)) return false;
  }

  uint32_t n_containers = 0;
  if (!rd(&n_containers, 4) || n_containers > 1000000u) return false;
  tmp.containers.resize(n_containers);
  for (uint32_t i = 0; i < n_containers; ++i) {
    uint16_t len = 0;
    if (!rd(&len, 2) || len > 4096u) return false;
    tmp.containers[i].resize(len);
    if (!rd(tmp.containers[i].data(), len)) return false;
  }

  uint32_t n_placements = 0;
  if (!rd(&n_placements, 4) || n_placements > 10000000u) return false;
  tmp.placements.resize(n_placements);
  for (uint32_t i = 0; i < n_placements; ++i) {
    auto& p = tmp.placements[i];
    uint8_t flags = 0;
    uint16_t name_len = 0;
    if (!rd(&flags, 1) || !rd(&p.route_list, 2) || !rd(&name_len, 2) ||
        !rd(&p.object_id, 4) || !rd(&p.container, 4) ||
        !rd(&p.placement.linear, sizeof(p.placement.linear)) ||
        !rd(&p.placement.offset, sizeof(p.placement.offset))) {
      return false;
    }
    p.store = (flags & 1u) ? PropStore::kPvsz : PropStore::kPgeo;
    p.has_object = (flags & 2u) != 0;
    if (name_len > 0) {
      if (name_len > 4096u) return false;
      p.name.resize(name_len);
      if (!rd(p.name.data(), name_len)) return false;
    }
  }

  uint32_t n_trees = 0;
  if (!rd(&n_trees, 4) || n_trees > 10000000u) return false;
  tmp.trees.resize(n_trees);
  for (uint32_t i = 0; i < n_trees; ++i) {
    auto& t = tmp.trees[i];
    if (!rd(&t.base, sizeof(t.base)) || !rd(&t.axis, sizeof(t.axis)) ||
        !rd(&t.u0, 4) || !rd(&t.v0, 4) || !rd(&t.u1, 4) || !rd(&t.v1, 4) ||
        !rd(&t.half_width, 4) || !rd(&t.height, 4) || !rd(&t.near, 4) || !rd(&t.far, 4) ||
        !rd(&t.tint, sizeof(t.tint)) || !rd(&t.atlas, 4) || !rd(&t.route_list, 2)) {
      return false;
    }
  }

  if (tmp.placements.empty() && tmp.trees.empty()) return false;
  out = std::move(tmp);
  return true;
}

bool save_props_cache(const std::filesystem::path& path, uint64_t zip_size, const PropWorld& in) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp_path = path.string() + ".tmp";
  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  auto wr = [&](const void* src, size_t n) -> bool {
    return n == 0 || bool(out.write(reinterpret_cast<const char*>(src), std::streamsize(n)));
  };
  if (!wr("SHPR0001", 8) || !wr(&zip_size, 8)) return false;

  const uint32_t n_routes = uint32_t(in.route_lists.size());
  if (!wr(&n_routes, 4)) return false;
  for (const auto& rl : in.route_lists) {
    const uint16_t len = uint16_t(rl.size());
    if (!wr(&len, 2) || !wr(rl.data(), len)) return false;
  }

  const uint32_t n_containers = uint32_t(in.containers.size());
  if (!wr(&n_containers, 4)) return false;
  for (const auto& c : in.containers) {
    const uint16_t len = uint16_t(c.size());
    if (!wr(&len, 2) || !wr(c.data(), len)) return false;
  }

  const uint32_t n_placements = uint32_t(in.placements.size());
  if (!wr(&n_placements, 4)) return false;
  for (const auto& p : in.placements) {
    const uint8_t flags = uint8_t((p.store == PropStore::kPvsz ? 1u : 0u) | (p.has_object ? 2u : 0u));
    const uint16_t name_len = uint16_t(p.name.size());
    if (!wr(&flags, 1) || !wr(&p.route_list, 2) || !wr(&name_len, 2) ||
        !wr(&p.object_id, 4) || !wr(&p.container, 4) ||
        !wr(&p.placement.linear, sizeof(p.placement.linear)) ||
        !wr(&p.placement.offset, sizeof(p.placement.offset)) ||
        !wr(p.name.data(), name_len)) {
      return false;
    }
  }

  const uint32_t n_trees = uint32_t(in.trees.size());
  if (!wr(&n_trees, 4)) return false;
  for (const auto& t : in.trees) {
    if (!wr(&t.base, sizeof(t.base)) || !wr(&t.axis, sizeof(t.axis)) ||
        !wr(&t.u0, 4) || !wr(&t.v0, 4) || !wr(&t.u1, 4) || !wr(&t.v1, 4) ||
        !wr(&t.half_width, 4) || !wr(&t.height, 4) || !wr(&t.near, 4) || !wr(&t.far, 4) ||
        !wr(&t.tint, sizeof(t.tint)) || !wr(&t.atlas, 4) || !wr(&t.route_list, 2)) {
      return false;
    }
  }
  out.flush();
  if (!out) return false;
  out.close();
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp_path, path, ec);
  }
  return !ec;
}

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
    const uint16_t tile_route_list = out.intern_routes(tile.routes);
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
      t.route_list = tile_route_list;
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
        p.route_list = tile_route_list;
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
      if (rec.extended) p.route_list = out.intern_routes(rec.extra);
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
  if (const auto it = first_name_cache_.find(member); it != first_name_cache_.end()) {
    family = it->second.first;
    level = it->second.second;
    return !family.empty();
  }
  const RenderModel* m = model(member);
  if (!m || m->objects.empty()) {
    first_name_cache_.emplace(member, std::pair<std::string, int>{});
    return false;
  }
  rmb_lod_part(m->objects.front().name, family, level);
  first_name_cache_.emplace(member, std::pair<std::string, int>{family, level});
  return true;
}

void PropModels::trim(size_t max_entries) {
  if (models_.size() <= max_entries) return;
  for (auto it = models_.begin(); it != models_.end(); ) {
    if (it->second) {
      if (first_name_cache_.find(it->first) == first_name_cache_.end()) {
        if (!it->second->objects.empty()) {
          std::string fam;
          int lvl = -1;
          rmb_lod_part(it->second->objects.front().name, fam, lvl);
          first_name_cache_.emplace(it->first, std::pair<std::string, int>{std::move(fam), lvl});
        } else {
          first_name_cache_.emplace(it->first, std::pair<std::string, int>{});
        }
      }
      it = models_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<uint32_t> PropModels::lod_set(uint32_t member) {
  std::string family;
  int level;
  if (!first_name(member, family, level)) return {};
  if (level < 0) return {member};
  auto matches_neighbor = [&](uint32_t cur, const std::string& cur_family, uint32_t cand,
                              int expected_level, std::string& cand_family) -> bool {
    int k = -1;
    if (!first_name(cand, cand_family, k) || k != expected_level) return false;
    if (cand_family == cur_family) return true;
    const std::string_view p0 = lod_prefix(cur_family);
    const std::string_view p1 = lod_prefix(cand_family);
    if (p0.empty() || p0 != p1) return false;
    const RenderModel* ma = model(cur);
    const RenderModel* mb = model(cand);
    return ma && mb && lod_boxes_compatible(*ma, *mb);
  };
  // Down to LOD00, then up while the next member continues the count.
  uint32_t low = member;
  std::string cur_family = family;
  for (int l = level; l > 0 && low > 0; --l) {
    std::string f;
    if (!matches_neighbor(low, cur_family, low - 1, l - 1, f)) break;
    --low;
    cur_family = std::move(f);
  }
  std::string low_family;
  int low_level;
  first_name(low, low_family, low_level);
  std::vector<uint32_t> set{low};
  cur_family = std::move(low_family);
  for (int l = low_level + 1;; ++l) {
    std::string f;
    const uint32_t next = set.back() + 1;
    if (!matches_neighbor(set.back(), cur_family, next, l, f)) break;
    set.push_back(next);
    cur_family = std::move(f);
  }
  return set;
}

int64_t PropModels::lod_member(uint32_t member, int level) {
  const auto set = lod_set(member);
  if (set.empty()) return -1;
  return set[std::min<size_t>(size_t(std::max(level, 0)), set.size() - 1)];
}

}  // namespace fh1
