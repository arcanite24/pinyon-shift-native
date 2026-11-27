// Every local-space placement of Colorado, from the kind-7 .pgeo tiles and
// the .pvsz zones of bin.zip, resolved through the object table to render
// models and their LOD sets.
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md,
// "Placements of local-space models", "Object table" and "Questions from
// the implementation side").
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fh1data/members.h"
#include "fh1data/placements.h"
#include "fh1data/rmb.h"

namespace fh1 {

enum class PropStore : uint8_t { kPgeo, kPvsz };

struct PropPlacement {
  PropStore store = PropStore::kPgeo;
  uint32_t object_id = 0;
  bool has_object = true;  // false: a .pgeo model record with an empty list 0
  uint32_t container = 0;  // index into PropWorld::containers (tile or zone name)
  Placement placement;     // file frame
  // Interned index into PropWorld::route_lists (0: empty list, shown on every
  // route). WORLD-GEOMETRY, "Which placements free roam draws".
  uint16_t route_list = 0;
  // The .pvsz record's name (its GameplayID, e.g. flyer_001), lower case;
  // empty for unnamed records and .pgeo placements.
  std::string name;
};

// Whether a placement with this route list is shown on `route` (0: free
// roam): it has no list, or its list holds the route.
inline bool shown_on_route(const std::vector<uint8_t>& routes, int route) {
  if (routes.empty()) return true;
  for (uint8_t r : routes)
    if (r == route) return true;
  return false;
}

// A tree sprite from a kind-7 tile's billboard records, placed (WORLD-
// MATERIALS, "Tree billboards (engine question Q38)"). The generated
// records of placed instances are not made (ours, for now).
struct TreeSprite {
  std::array<float, 3> base{};  // game frame (the file frame with z negated), the drive's world frame
  std::array<float, 3> axis{};  // trunk axis, game frame
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  float half_width = 0, height = 0, near = 0, far = 0;
  std::array<uint8_t, 4> tint{};  // R, G, B, A as stored
  uint32_t atlas = 0;             // texture record (the group's reference value)
  uint16_t route_list = 0;        // interned index into PropWorld::route_lists (0: empty)
};

struct PropWorld {
  std::vector<std::string> containers;
  // Interned distinct route lists; index 0 is always empty (shown on every route).
  std::vector<std::vector<uint8_t>> route_lists{{}};
  std::vector<PropPlacement> placements;  // every .pgeo instance, every distinct .pvsz object
  std::vector<TreeSprite> trees;          // every billboard record whose group names a reference
  uint64_t tree_unresolved = 0;           // records whose group key is past the reference list

  uint16_t intern_routes(const std::vector<uint8_t>& routes);
  bool shown_on_route(uint16_t list_idx, int route) const {
    if (list_idx == 0 || list_idx >= route_lists.size()) return true;
    return fh1::shown_on_route(route_lists[list_idx], route);
  }

  // .pgeo census
  std::map<uint32_t, uint64_t> pgeo_kinds;
  uint64_t pgeo_tiles = 0, pgeo_parsed = 0, pgeo_failed = 0, pgeo_models = 0, pgeo_groups = 0;
  uint64_t pgeo_instances = 0, pgeo_consecutive_records = 0, pgeo_records_without_list0 = 0;
  uint64_t pgeo_trailing_matches = 0;  // tiles whose trailing block is 40 x the groups' counts
  std::map<size_t, uint64_t> pgeo_header_sizes;
  // .pvsz census
  uint64_t pvsz_zones = 0, pvsz_parsed = 0, pvsz_failed = 0, pvsz_records = 0;
  uint64_t pvsz_multi_start = 0, pvsz_not_first_start = 0;
  uint64_t pvsz_conflicts = 0;        // records that differ from the object's first record
  uint64_t pvsz_first_conflicts = 0;  // the same with the spec's first-start rule
  uint64_t pvsz_flagged_ids = 0, pvsz_max_object = 0;
  std::vector<std::string> failures;  // "<member>: <reason>"
};

struct PropLoadOptions {
  bool census_first_rule = true; // also count cross-zone conflicts under the first-start rule
};

// Reads every .pgeo and .pvsz member of `source`.
bool load_props(MemberSource& source, PropWorld& out, std::string* error,
                const PropLoadOptions& options = {});

// Binary disk cache for PropWorld placements, trees, and interned route lists.
bool load_props_cache(const std::filesystem::path& path, uint64_t zip_size, PropWorld& out);
bool save_props_cache(const std::filesystem::path& path, uint64_t zip_size, const PropWorld& in);

// Render models by member number, parsed once, with LOD sets: the run of
// consecutive members whose first mesh names differ only in a "_LODnn"
// part counting up by one (a model without one is a set of one).
class PropModels {
 public:
  // `prefix`: the world's model prefix, its folder name in lower case and
  // "out" (coloradoout, coloradodirtout).
  explicit PropModels(MemberSource& source, std::string prefix = "coloradoout")
      : source_(source), prefix_(std::move(prefix)) {}

  static std::string member_name(uint32_t member, const std::string& prefix = "coloradoout");  // <prefix>.NNNNN.rmb.bin
  std::string name_of(uint32_t member) const { return member_name(member, prefix_); }

  // The parsed model, or null (missing or failing; `error` says which).
  const RenderModel* model(uint32_t member, std::string* error = nullptr);

  // Pre-reads and parses a batch of uncached member models in parallel.
  void preload(const std::vector<uint32_t>& members);

  // The set's member at `level` (0 = LOD00); past the set's last member,
  // its last. Returns -1 when `member` itself is unavailable.
  int64_t lod_member(uint32_t member, int level);

  // The members of `member`'s LOD set, lowest level first.
  std::vector<uint32_t> lod_set(uint32_t member);

  // The family and LOD level of the model's first mesh name ("_LODnn"; -1
  // without one). False when the model is unavailable.
  bool first_name(uint32_t member, std::string& family, int& level);

  // Evicts cached RenderModel meshes when the map exceeds `max_entries`
  // (preserving lightweight first_name metadata so tree/marker checks stay O(1)).
  void trim(size_t max_entries);

 private:
  MemberSource& source_;
  std::string prefix_;
  std::map<uint32_t, std::unique_ptr<RenderModel>> models_;
  std::map<uint32_t, std::string> errors_;
  std::unordered_map<uint32_t, std::pair<std::string, int>> first_name_cache_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> lod_set_cache_;
};

}  // namespace fh1
