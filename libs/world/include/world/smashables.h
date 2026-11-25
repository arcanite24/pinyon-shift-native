// Prop physics definitions, 384-body dynamic smashable prop pool, movable event
// props (CollObjects.xml), and barn-door open/closed collision swap.
//
// Spec: FORMAT-PHYSICS-DEFINITIONS (horizon1-recomp docs/formats/PHYSICS_DEFINITIONS.md).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "drive/vehicle.h"
#include "world/collision.h"
#include "world/world.h"

namespace world {

enum class PropShapeKind : uint8_t {
  kSphere = 0,
  kObb = 1,
  kConvexHull = 2,
};

struct PropShape {
  PropShapeKind kind = PropShapeKind::kSphere;
  uint8_t flags = 3;
  // Sphere: centre (x, y, z), radius.
  std::array<float, 4> sphere{};
  // OBB & ConvexHull: local 4x4 transform matrix.
  std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // OBB: half-extents (hx, hy, hz, w).
  std::array<float, 4> half_extents{};
  // ConvexHull geometry.
  std::vector<std::array<float, 3>> hull_verts;
  std::vector<std::array<uint8_t, 2>> hull_edges;
  std::vector<std::vector<uint8_t>> hull_faces;
};

struct PropPhysicsDef {
  int32_t version = 13;
  int32_t definition_type = 0;  // 0 = track collidable prop, 1 = vehicle
  float mass = 0.1f;            // 100 kg units (multiply by 100 for kg)
  std::array<float, 9> inertia_tensor{};
  std::array<float, 9> inv_inertia_tensor{};
  std::array<float, 3> half_extents{0.5f, 0.5f, 0.5f};
  float bounding_radius = 0.8f;
  std::array<float, 3> graphics_offset{};
  int32_t num_children = 0;
  std::array<float, 3> aabb_centre_offset{};
  // DefinitionType == 0 fields:
  int32_t legacy_render_model_index = -1;
  float visible_distance = 200.0f;
  int32_t surface_type_index = -1;
  float break_speed_mph = 0.0f;  // 0 = any contact; 9999 = unbreakable sentinel
  int32_t unique_index = 0;
  uint32_t object_name_hash = 0;
  int8_t object_type_index = -1;  // -1 generic, 0 Fence, 1 Flyer, 2 Mailbox, 3 RaceActivation
  std::vector<PropShape> shapes;
  std::vector<PropPhysicsDef> children;

  bool unbreakable() const { return break_speed_mph >= 9998.0f; }
  float break_speed_mps() const { return break_speed_mph * 0.44704f; }
  float mass_kg() const { return std::max(0.5f, mass * 100.0f); }
};

struct PhysicsDefinitionsCounts {
  size_t top_level = 0;
  size_t max_global_index = 0;
  size_t total_children = 0;
  size_t parents_with_children = 0;
  size_t max_children = 0;
  size_t spheres = 0;
  size_t obbs = 0;
  size_t hulls = 0;
  std::map<int, size_t> by_object_type;
};

struct PhysicsDefinitions {
  std::vector<PropPhysicsDef> definitions;
  std::vector<uint16_t> global_to_list;
  std::vector<std::vector<int32_t>> child_render_models;  // indexed by global_index

  // Resolves a packed 32-bit ID: high 16 = GlobalIndex, low 16 = ChildIndex (0xFFFF = parent).
  const PropPhysicsDef* resolve_packed(uint32_t packed_id) const;
  // Finds the first top-level definition matching `object_type_index` (0 Fence, 1 Flyer, 2 Mailbox, 3 RaceActivation).
  const PropPhysicsDef* find_by_object_type(int8_t object_type_index) const;
  PhysicsDefinitionsCounts counts() const;
};

// Parses big-endian PhysicsDefinitions.bin to exact EOF.
bool parse_physics_definitions(std::span<const uint8_t> bytes, PhysicsDefinitions& out, std::string* error = nullptr);
bool load_physics_definitions_file(const std::filesystem::path& path, PhysicsDefinitions& out,
                                   std::string* error = nullptr);

// Movable event prop from physics.zip/CollObjects.xml (31 types).
struct CollObjectType {
  std::string name;
  struct Sphere {
    std::array<float, 3> centre{};
    float radius = 0.1f;
  };
  std::vector<Sphere> spheres;
  float total_mass = 1.0f;
  int surface_id = 0;
  bool is_trigger = false;
};

bool parse_coll_objects_xml(std::string_view xml, std::vector<CollObjectType>& out);
bool parse_smashable_object_types_xml(std::string_view xml, std::vector<std::string>& out);

// One placed collidable prop from Ribbon_00/CollObjs.xml (game frame).
struct CollObjPlacement {
  std::string physics_type;  // e.g. "CO_CLRD_FenceF.0.rmb"
  std::string stem;          // lower-case stem before first '.', e.g. "co_clrd_fencef"
  std::array<float, 3> pos{};
  std::array<float, 3> x_axis{1, 0, 0};
  std::array<float, 3> y_axis{0, 1, 0};
  std::array<float, 3> z_axis{0, 0, 1};
};

bool parse_coll_objs_xml(std::string_view xml, std::vector<CollObjPlacement>& out);

// Barn-door open/closed collision entry (barnfinds.xml DoorInfo + GameObjs.xml).
struct BarnDoorCollider {
  std::string barn_name;
  std::string closed_coll_id;
  std::string open_coll_id;
  std::array<float, 3> pos{};  // game frame
  std::array<float, 3> x_axis{1, 0, 0};
  std::array<float, 3> y_axis{0, 1, 0};
  std::array<float, 3> z_axis{0, 0, 1};
  bool is_open = false;        // false = closed_coll_id active; true = open_coll_id active
};

// Solid unbreakable barrier / festival crowd-control barricade OBB collider (game frame).
struct SolidBarrier {
  std::array<float, 3> pos{};            // game-frame placement origin
  std::array<float, 3> x_axis{1, 0, 0};  // unit local axes in game frame
  std::array<float, 3> y_axis{0, 1, 0};
  std::array<float, 3> z_axis{0, 0, 1};
  std::array<float, 3> local_center{};   // OBB center in prop local game frame
  std::array<float, 3> half_extents{1.25f, 0.75f, 0.20f};
  std::vector<uint8_t> routes;           // empty = shown on every route; else route IDs (0 = free roam)
  float radius = 1.5f;
};

struct SmashEvent {
  std::array<float, 3> pos{};
  std::array<float, 3> impulse_vel{};
  std::string prop_stem;
  int8_t object_type_index = -1;  // 0 Fence, 1 Flyer, 2 Mailbox, 3 RaceActivation
  int32_t surface_type_index = -1;
  int32_t global_index = -1;      // PhysicsDefinitions GlobalIndex (0..MaxGlobalIndex-1), or -1
  int debris_spawned = 0;
  float hide_radius = 2.0f;       // radius (m) to mask out intact static world prop
};

// Render-ready vertex for dynamic flying smashable debris (game frame).
// Real FH1 child/prop .rmb.bin debris carries UV + PVS texture_record + cutout flag;
// untextured fallback / world_anim callers leave texture_record == kNoTexture.
struct DebrisVertex {
  std::array<float, 3> pos{};
  std::array<float, 3> normal{0, 1, 0};
  std::array<uint8_t, 4> rgba{200, 180, 150, 255};
  std::array<float, 2> uv{0.0f, 0.0f};
  uint32_t texture_record = kNoTexture;
  uint8_t cutout = 0;
};

// Runtime 384-body dynamic smashable prop pool + barn-door collision manager.
class SmashableWorld {
 public:
  static constexpr size_t kMaxDynamicBodies = 384;  // confirmed pool cap (FORMAT-PHYSICS-DEFINITIONS)
  static constexpr float kDebrisDespawnSeconds = 9.0f;
  static constexpr float kRespawnCheckIntervalS = 1.0f;
  static constexpr float kRespawnDistanceM = 300.0f;

  SmashableWorld();
  ~SmashableWorld();
  SmashableWorld(SmashableWorld&&) noexcept;
  SmashableWorld& operator=(SmashableWorld&&) noexcept;
  SmashableWorld(const SmashableWorld&) = delete;
  SmashableWorld& operator=(const SmashableWorld&) = delete;

  // Loads PhysicsDefinitions.bin, SmashableObjectTypes.xml, physics.zip/CollObjects.xml,
  // Ribbon_00/CollObjs.xml, and real FH1 smashable .rmb.bin debris models + .pvsz GlobalIndex links.
  bool load(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
            std::string* error = nullptr);

  // Active track route filter for .pvsz smashables (0 = free roam, -1 = all routes).
  void set_active_route(int route) { active_route_ = route; }

  // Decodes a world texture record referenced by a real smashable debris mesh from bin.zip.
  bool read_texture(uint32_t record, fh1::WorldTexture& out, std::string* error = nullptr) const;

  // Registers barn-door colliders from GameObjs.xml (matching `closed_coll_id` / `open_coll_id`).
  bool load_barn_doors(const std::filesystem::path& game_objs_path,
                       const std::vector<BarnDoorCollider>& barn_specs);

  // Updates each barn's open/closed collision state from discovered barn names / hidden set.
  void set_barn_open(std::string_view barn_name, bool open);
  void sync_barns_from_hidden(const std::set<std::string>& hidden_ids);

  // Synthetic setup helpers for unit tests.
  void set_definitions(PhysicsDefinitions defs) { defs_ = std::move(defs); }
  void add_prop(const CollObjPlacement& p, const PropPhysicsDef* explicit_def = nullptr);
  void add_barn_door(const BarnDoorCollider& door) { barn_doors_.push_back(door); }
  void add_solid_barrier(const SolidBarrier& barrier);

  // Breaks the props `vehicle` hits at or above their BreakSpeedMph, steps
  // the barn doors and all active dynamic debris. The props it does not
  // break, and the solid barriers, push back through the car's body solver
  // instead (SmashableGround).
  void step(drive::Vehicle& vehicle, const drive::Ground& ground, float dt, std::vector<SmashEvent>& events);

  // The contact rows for an intact prop and for a solid barrier
  // (CategoryToCategoryParams Smashable/Car and Car/GuardRail); null keeps
  // the shipped values.
  void set_contact_params(const CategoryCollisionParams* prop, const CategoryCollisionParams* barrier);
  // Whether load() found both rows in physics.zip (else the shipped values).
  bool contact_rows_loaded() const { return contact_rows_loaded_; }
  // Adds, for each of `n` body spheres, its contact with an intact prop the
  // car would not break at `car_speed_mps`, or with a solid barrier, where
  // that is deeper than `hits[i]` (FEEL-21). The props are fixed bodies.
  void add_sphere_contacts(const drive::Vec3& inside, double car_speed_mps, const drive::SphereQuery* spheres,
                           int n, drive::SphereHit* hits) const;

  // Explicitly shatters a discount sign (`Flyer`, 12 debris children) at `pos`.
  int spawn_flyer_shatter(const std::array<float, 3>& pos, const std::array<float, 3>& car_vel);
  int spawn_flyer_shatter(const std::array<float, 3>& pos, const std::array<float, 3>& x_axis,
                          const std::array<float, 3>& y_axis, const std::array<float, 3>& z_axis,
                          const std::array<float, 3>& car_vel);
  // Persists a broken external prop (e.g. smashed discount sign) in broken_hidden_positions().
  void record_hidden_prop(const std::array<float, 3>& pos, float hide_radius);

  // Clears all flying debris and restores non-Discount-Sign broken props (e.g. on event start/restart).
  void clear_debris();
  void reset_broken();
  // Drains world hidden spheres for props recently restored via distance respawn or reset_broken().
  std::vector<GatherOptions::HiddenSphere> consume_respawned_hidden();

  // Rewind snapshot/restore: captures broken prop indices so props broken after
  // the rewind keyframe are restored (and returns their hidden spheres to unhide).
  struct RewindSnapshot {
    std::vector<uint32_t> broken_indices;
  };
  RewindSnapshot capture_rewind() const;
  std::vector<GatherOptions::HiddenSphere> restore_rewind(const RewindSnapshot& s);

  // Exports lit 3D triangle mesh for all active flying debris bodies.
  void build_debris_mesh(std::vector<DebrisVertex>& out_tris) const;

  // Queries & diagnostics.
  const PhysicsDefinitions& definitions() const { return defs_; }
  const std::vector<CollObjectType>& coll_object_types() const { return coll_types_; }
  const std::vector<std::string>& smashable_object_types() const { return smash_types_; }
  const std::vector<BarnDoorCollider>& barn_doors() const { return barn_doors_; }
  size_t placed_prop_count() const { return raw_xml_prop_count_ ? raw_xml_prop_count_ : props_.size(); }
  size_t unique_prop_count() const { return props_.size(); }
  size_t solid_barrier_count() const { return solid_barriers_.size(); }
  size_t loaded_debris_model_count() const;
  size_t matched_global_index_count() const;
  size_t active_dynamic_count() const { return dynamic_bodies_.size(); }
  size_t broken_prop_count() const { return broken_positions_ ? broken_positions_->size() : 0; }
  std::shared_ptr<const std::vector<GatherOptions::HiddenSphere>> broken_hidden_positions() const {
    return broken_positions_;
  }
  uint64_t total_evictions() const { return evictions_; }

  // Evaluates the BreakSpeedMph rule (exposed for unit tests).
  static bool should_break(const PropPhysicsDef& def, float impact_speed_mps);
  // Classifies whether a static render model represents a solid unbreakable festival/crowd barrier.
  static bool is_solid_barrier_model(std::string_view object_name, const std::array<float, 3>& box_min,
                                     const std::array<float, 3>& box_max);

 private:
  struct Impl;
  struct PlacedProp {
    CollObjPlacement place;
    const PropPhysicsDef* def = nullptr;
    int32_t global_index = -1;       // exact GlobalIndex into PhysicsDefinitions (0..MaxGlobalIndex-1)
    int32_t intact_member = -1;      // intact coloradoout.NNNNN.rmb.bin member (fallback debris mesh)
    std::vector<uint8_t> routes;     // empty = shown on every route; else route IDs (0 = free roam)
    float radius = 0.8f;
    bool broken = false;
  };
  struct DynamicBody {
    std::array<float, 3> pos{};
    std::array<float, 3> vel{};
    drive::Quat rot{};
    std::array<float, 3> omega{};
    std::array<float, 3> half_extents{0.25f, 0.25f, 0.08f};
    std::array<float, 3> mesh_offset{};  // local game-frame CoM shift applied to real .rmb.bin vertices
    std::array<uint8_t, 4> colour{185, 145, 95, 255};
    int32_t render_model_member = -1;    // real coloradoout.NNNNN.rmb.bin member (-1 = procedural box fallback)
    float mass_kg = 10.0f;
    float age = 0.0f;
    bool settled = false;
  };

  const PropPhysicsDef* match_definition(const CollObjPlacement& p) const;
  bool is_discount_sign(const PlacedProp& pp) const;
  float hide_radius_for(const PlacedProp& pp) const;
  void rebuild_broken_positions();
  void spawn_dynamic_body(const DynamicBody& body);
  std::array<uint8_t, 4> colour_for_prop(int32_t surface_type, int8_t object_type, std::string_view stem) const;
  void step_barn_doors(drive::Vehicle& vehicle);
  static int64_t cell_key(float x, float z);

  std::unique_ptr<Impl> impl_;
  drive::SurfaceTyre prop_contact_, barrier_contact_;
  bool contact_rows_loaded_ = false;
  PhysicsDefinitions defs_;
  std::vector<CollObjectType> coll_types_;
  std::vector<std::string> smash_types_;
  std::vector<PlacedProp> props_;
  size_t raw_xml_prop_count_ = 0;
  int active_route_ = 0;
  std::unordered_map<int64_t, std::vector<uint32_t>> grid_;  // 25 m XZ spatial grid
  std::vector<SolidBarrier> solid_barriers_;
  std::unordered_map<int64_t, std::vector<uint32_t>> barrier_grid_;  // 25 m XZ spatial grid
  std::vector<DynamicBody> dynamic_bodies_;                  // capped at kMaxDynamicBodies (384)
  std::vector<BarnDoorCollider> barn_doors_;
  std::vector<GatherOptions::HiddenSphere> persistent_hidden_;  // external persistent hides (Discount Signs)
  std::vector<GatherOptions::HiddenSphere> respawned_hidden_;   // queued unhide spheres for renderer/streamer
  std::shared_ptr<std::vector<GatherOptions::HiddenSphere>> broken_positions_;
  float respawn_timer_ = 0.0f;
  uint64_t evictions_ = 0;
  uint32_t rng_ = 0x51C4D3B7u;

  float rand01();
  float rand_sym();
};

// A ground for the car's step that adds the smashables to the world it
// stands on: wheel rays go to `base`, body spheres meet `base` and the
// props and barriers of `world` (SmashableWorld::add_sphere_contacts).
class SmashableGround : public drive::Ground {
 public:
  SmashableGround(const drive::Ground& base, const SmashableWorld& world, double car_speed_mps)
      : base_(base), world_(world), car_speed_(car_speed_mps) {}
  bool raycast(const drive::Vec3& origin, const drive::Vec3& dir, double max_distance,
               drive::Hit& hit) const override {
    return base_.raycast(origin, dir, max_distance, hit);
  }
  void sphere_contacts(const drive::Vec3& inside, const drive::SphereQuery* spheres, int n,
                       drive::SphereHit* hits) const override {
    base_.sphere_contacts(inside, spheres, n, hits);
    world_.add_sphere_contacts(inside, car_speed_, spheres, n, hits);
  }

 private:
  const drive::Ground& base_;
  const SmashableWorld& world_;
  double car_speed_;
};

}  // namespace world
