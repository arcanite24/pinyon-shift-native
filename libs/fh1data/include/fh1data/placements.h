// Placements of Colorado's local-space render models (props, signs,
// fences, trees, barriers), and the object table that names their models.
//
// Three stores, all big-endian and in the game frame (the file frame with z
// negated):
//   - the object table in Ribbon_00/Colorado_00.pvs: per object ID, the
//     member number of its render model, coloradoout.NNNNN.rmb.bin;
//   - kind-7 `__R00GNNNNN.pgeo` tiles in bin.zip: model records naming
//     object IDs, groups of 96-byte instances with f32 axis rows;
//   - `__R00ZNNNNN.pvsz` zone files in bin.zip: object IDs, then one
//     placement record per ID (f32 position, half-float rotation).
// Every reader returns false with a reason on a layout mismatch.
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md,
// "Placements of local-space models" and "Object table").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fh1 {

// IEEE 754 binary16 to float (subnormals, infinities and NaN included).
float half_to_float(uint16_t bits);

// --- object table (Colorado_00.pvs) -----------------------------------------

struct PvsObject {
  uint16_t member = 0;  // render model coloradoout.<member>.rmb.bin
  // Not decoded: a signed u16, two flag-like u16s, a u32 (0 or an index), a
  // u32 (-1 or a small count), a u16 (usually all ones).
  int16_t field_2 = 0;
  uint16_t field_4 = 0, field_6 = 0;
  uint32_t field_8 = 0, field_12 = 0;
  uint16_t field_16 = 0;
};

// A texture record of the 28-byte table (WORLD-MATERIALS, "From a texture
// slot to a file"): the texture ID names its files.
struct PvsTexture {
  uint32_t id = 0;    // _0x%08X.bix with _0x%08X_B.bix (type 4), _0x%08X.bin (type 0)
  uint8_t flag = 0;   // 1, or 0 on 75 records (not needed to find the file)
  uint8_t type = 0;   // 4 .bix pair, 0 CAFF .bin, 1 and 3 no file
};
enum : uint8_t { kPvsTextureCaff = 0, kPvsTextureNone = 1, kPvsTexturePlaceholder = 3, kPvsTextureBix = 4 };

struct PvsObjectTable {
  uint32_t version = 0;               // 50 on the disc
  size_t record_table_offset = 0;     // the u32 count of the 28-byte records
  uint32_t record_count = 0;          // 28-byte records (texture/material list, presumably)
  std::vector<std::string> shaders;   // the track shader names
  size_t object_table_offset = 0;     // the u32 count of the 18-byte records
  std::vector<PvsObject> objects;     // indexed by object ID
  std::vector<PvsTexture> textures;   // the 28-byte records, by record index
  // The per-model texture lists after the object table: list j belongs to
  // render model member j + 1; a material's slot value i is entry i.
  std::vector<std::vector<uint32_t>> model_textures;
  std::string texture_list_error;     // why the lists did not read (they are then empty)
};

// Reads the 28-byte record table, the shader names and the object table.
// The bytes between the version and the 28-byte table are not described by
// the spec; the table is located as the first offset from which the
// layout holds: a count n, then n records numbered 0..n-1 whose two f32 are
// 1.0 and whose ten reserved bytes are zero, then a counted list of
// printable names, then the object count and its records inside the file.
// Then the per-model texture lists (a count, three u32 2, 0, 1, and per
// list: k effect indices, a 15-float box, t texture record indices).
bool parse_object_table(std::span<const uint8_t> data, PvsObjectTable& out, std::string* error);

// --- kind-7 .pgeo tiles -------------------------------------------------------

struct PgeoInstance {
  // Each row is the image of one local axis in the game frame, scale
  // included. Placement in the file frame: F * R^T * F * v + F * t.
  std::array<std::array<float, 3>, 3> rows{};
  std::array<float, 3> translation{};  // game frame
  float w = 0;                         // 1.0
  std::array<float, 4> colour{};       // colour-like, not decoded
  std::array<float, 3> normal{};       // normal-like, not decoded
  uint32_t word = 0;                   // not decoded
};

struct PgeoModelRecord {
  std::array<float, 3> box_min{}, box_max{};
  std::string text;                           // string bytes (a source path), often empty
  std::array<std::vector<uint32_t>, 3> ids{};  // object IDs per LOD list (probable)
  uint32_t word = 0;                          // not decoded
};

struct PgeoGroup {
  uint32_t key = 0;           // index into PgeoTile::references (a texture record for the sprites)
  uint32_t model_record = 0;  // index into PgeoTile::models
  uint32_t packed_count = 0;  // 40-byte records (in the tile's trailing block)
  uint32_t record32_count = 0;  // 32-byte records after the instances (not decoded)
  std::array<float, 4> distances{};  // the first: where a placed model gives way to its sprite
  // Tree sprites: the atlas rectangle (u0, u1, v1, v0) and the sprite's
  // width and height in metres.
  std::array<float, 4> atlas_rect{};
  std::array<float, 2> sprite_size{};
  std::vector<PgeoInstance> instances;
};

// One 40-byte tree billboard record of the trailing block (WORLD-MATERIALS,
// "Tree billboards (engine question Q38)").
struct PgeoBillboard {
  uint32_t group = 0;                 // the group it belongs to (records are in group order)
  uint32_t position_word = 0;         // +0 as stored (big-endian)
  std::array<float, 3> base{};        // tile frame: game frame, the tile box's minimum as origin
  std::array<float, 3> axis{};        // trunk axis U, y up (signed 10_11_11, not normalised)
  std::array<float, 3> normal{};      // lighting normal (2_10_10_10 signed)
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas cell: (u0, v0) top, (u1, v1) bottom
  float half_width = 0, height = 0, near = 0, far = 0;  // metres
  std::array<uint8_t, 4> tint{};      // R, G, B, A as stored A, R, G, B
  std::array<uint8_t, 4> extra{};     // the extra light colour, R, G, B, A
};

struct PgeoTile {
  uint32_t kind = 0;  // 7
  std::array<float, 3> box_min{}, box_max{};  // game frame
  float distance = 0;                          // +0x38, a draw distance (guess)
  std::string name;
  size_t header_size = 0;  // 0x60, or 0x98 when the word at +0x58 is not zero
  // With the longer header: the byte count at +0x84 and the route list
  // after it (WORLD-GEOMETRY, "Which placements free roam draws").
  bool has_routes = false;
  std::vector<uint8_t> routes;
  std::vector<std::pair<uint32_t, uint32_t>> references;  // (value, flag)
  uint32_t string_records = 0;                             // 8-byte records, skipped
  std::vector<PgeoModelRecord> models;
  std::vector<PgeoGroup> groups;
  size_t trailing_size = 0;  // the block of 40-byte packed records at the end
  std::vector<PgeoBillboard> billboards;  // the trailing block, decoded
};

// Decodes one 40-byte billboard record over a tile box of `size`.
PgeoBillboard decode_pgeo_billboard(const uint8_t* record, const std::array<float, 3>& size);

// The kind word of a .pgeo tile (2 to 8 on the disc), or 0 with a reason.
uint32_t pgeo_kind(std::span<const uint8_t> data, std::string* error);

// Parses a kind-7 tile to its stated size. Other kinds are refused.
bool parse_pgeo(std::span<const uint8_t> data, PgeoTile& out, std::string* error);

// --- .pvsz zone files ---------------------------------------------------------

struct PvszRecord {
  size_t offset = 0;
  std::array<float, 3> distances{};  // three half floats (LOD or fade distances, guess)
  std::array<float, 3> position{};   // game frame
  std::array<float, 9> rotation{};   // nine half floats h0..h8 in file order
  std::array<uint8_t, 16> unknown{};
  bool extended = false;  // the flag byte
  uint32_t word = 0;      // extended: -1, or a small per-model index
  std::vector<uint8_t> extra;  // extended: the counted bytes, the route list (WORLD-GEOMETRY, Q4)
  std::string name;            // extended: the 32-byte name, NULs stripped
};

enum class PvszStartRule {
  kFirst,      // the spec's rule: the first start that ends exactly at the end
  kMostRigid,  // of those starts, the one with the most rigid rotations (ties: first)
};

struct PvszZone {
  std::vector<uint32_t> ids;   // raw: the top bit is a flag (not decoded)
  std::vector<size_t> starts;  // every start from which ids.size() records end at the end
  size_t start = 0;            // the one taken
  std::vector<PvszRecord> records;  // records[k] for ids[k]
  size_t rigid_records = 0;         // records with a finite position and a rigid rotation
};

inline uint32_t pvsz_object_id(uint32_t raw) { return raw & 0x7FFFFFFFu; }

// Parses a zone. The block between the IDs and the records has no stored
// size, so every start offset from which the records end exactly at the
// end of the file is a candidate; `rule` picks one.
bool parse_pvsz(std::span<const uint8_t> data, PvszZone& out, std::string* error,
                PvszStartRule rule = PvszStartRule::kMostRigid);

// Whether a record's rotation is rigid to half-float precision (|H H^T - I|
// under 0.01 everywhere) and its position finite.
bool pvsz_record_rigid(const PvszRecord& record);

// --- placing models -----------------------------------------------------------

// file-frame position = linear * v + offset, for a model vertex v.
struct Placement {
  std::array<std::array<double, 3>, 3> linear{};
  std::array<double, 3> offset{};
  std::array<double, 3> apply(const std::array<float, 3>& v) const;
};

Placement pgeo_placement(const PgeoInstance& instance);
// The halves are the file-frame rotation's columns, each with its third
// component negated: R[r][c] = h[3c + r] * F[r]; the vertex v is placed at
// R * v + F * position.
Placement pvsz_placement(const PvszRecord& record);

// A render model's LOD part: the first "_LODnn" (any case) of a mesh object
// name. Returns false when there is none; else `family` is the name with
// the two digits replaced by "**" (lower case) and `level` is nn.
bool rmb_lod_part(const std::string& name, std::string& family, int& level);

}  // namespace fh1
