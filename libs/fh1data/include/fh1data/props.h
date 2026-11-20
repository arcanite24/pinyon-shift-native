// Every local-space placement of Colorado, from the kind-7 .pgeo tiles and
// the .pvsz zones of bin.zip, resolved through the object table to render
// models and their LOD sets.
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md,
// "Placements of local-space models", "Object table" and "Questions from
// the implementation side").
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
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
  // The route list of its .pvsz record or .pgeo tile (empty: none; shown
  // on every route). WORLD-GEOMETRY, "Which placements free roam draws".
  std::vector<uint8_t> routes;
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
  std::vector<uint8_t> routes;    // its tile's route list (empty: none)
};

struct PropWorld {
  std::vector<std::string> containers;
  std::vector<PropPlacement> placements;  // every .pgeo instance, every distinct .pvsz object
  std::vector<TreeSprite> trees;          // every billboard record whose group names a reference
  uint64_t tree_unresolved = 0;           // records whose group key is past the reference list

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

// Render models by member number, parsed once, with LOD sets: the run of
// consecutive members whose first mesh names differ only in a "_LODnn"
// part counting up by one (a model without one is a set of one).
class PropModels {
 public:
  explicit PropModels(MemberSource& source) : source_(source) {}

  static std::string member_name(uint32_t member);  // coloradoout.NNNNN.rmb.bin

  // The parsed model, or null (missing or failing; `error` says which).
  const RenderModel* model(uint32_t member, std::string* error = nullptr);

  // The set's member at `level` (0 = LOD00); past the set's last member,
  // its last. Returns -1 when `member` itself is unavailable.
  int64_t lod_member(uint32_t member, int level);

  // The members of `member`'s LOD set, lowest level first.
  std::vector<uint32_t> lod_set(uint32_t member);

  // The family and LOD level of the model's first mesh name ("_LODnn"; -1
  // without one). False when the model is unavailable.
  bool first_name(uint32_t member, std::string& family, int& level);

 private:

  MemberSource& source_;
  std::map<uint32_t, std::unique_ptr<RenderModel>> models_;
  std::map<uint32_t, std::string> errors_;
};

}  // namespace fh1
