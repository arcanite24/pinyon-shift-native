// FH1 `.carbin` car models (FORMAT-CARS, docs/formats/CARS.md in horizon1-recomp).
//
// Big-endian; after the first variable-length name nothing is aligned.
//
//   header (variable) | u32 part count | parts | footer (24 bytes)
//
// The header ends in the draw-list block (4, 4, n, n transparent lists, n
// opaque lists, 1), which sorts every sub-section into a transparent and an
// opaque list per LOD; the part count follows it. The block is found by the
// spec's scan over the whole file. Each part has a box (offset,
// half-extents), a name, a LOD vertex pool (LOD1-5), sub-sections (one per
// material and LOD, each an index buffer with a UV transform), a LOD0 vertex
// pool and a damage tail. Fields whose meaning is not known are kept as
// bytes, so `encode_carbin` writes an unchanged model back byte for byte.
// `stripped_*` twins are not read.
// Spec: FORMAT-CARS (.carbin: header, draw-list block, parts, pool, vertex,
// sub-sections, damage tail).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fh1 {

// The 28-byte vertex: position as a unit direction (xyz / 32767) and a
// length (w / 32767, a fraction of |half-extent|), UV0 and UV1 (/ 65535,
// before the sub-section's transform), a unit quaternion tangent frame
// (/ 32767, turns +X to the normal, +Y to the tangent along U, -Z to the
// tangent along V), four bytes of baked lighting (the first behaves as
// ambient occlusion, about 100 when open to the sky).
struct CarbinVertex {
  std::array<int16_t, 4> position;
  std::array<uint16_t, 2> uv0;
  std::array<uint16_t, 2> uv1;
  std::array<int16_t, 4> frame;
  std::array<uint8_t, 4> lighting;
};

struct CarbinPool {
  int32_t count = 0;          // signed: a few pools store a negative count (no vertices)
  uint32_t stride = 0;        // 28 for every pool with vertices
  uint32_t buffer_flag = 0;   // 0 no buffer, 1 a buffer with its data, 3 a stripped twin's
  std::optional<uint32_t> buffer_id;   // present when buffer_flag != 0
  std::vector<CarbinVertex> vertices;  // decoded when stride == 28
  std::vector<uint8_t> raw;            // the vertex bytes when stride != 28
  size_t raw_bytes = 0;                // count x stride
  size_t vertex_count() const { return count > 0 ? size_t(count) : 0; }
};

enum class CarbinIndexType : uint32_t { kTriangleList = 4, kTriangleStrip = 6 };

struct CarbinSubsection {
  std::array<uint8_t, 5> head{};       // not decoded
  // (U offset, U scale, V offset, V scale) for UV0, then the same for UV1.
  std::array<float, 8> uv_transform{};
  std::array<uint8_t, 36> unknown1{};  // not decoded
  std::string material;       // the car shader technique, e.g. "body", "window"
  int32_t lod = 0;            // 0 indexes the LOD0 pool, 1-5 the LOD pool
  uint32_t index_type = 0;    // CarbinIndexType
  std::array<uint8_t, 56> unknown2{};  // not decoded
  std::array<uint32_t, 2> version_words{};
  uint32_t index_size = 2;    // 2 or 4 bytes
  std::vector<uint32_t> indices;
  std::array<uint8_t, 4> end{};        // not decoded
};

struct CarbinDamageBlock {
  uint32_t kind = 0, count = 0, stride = 0, extra = 0;
  std::vector<uint8_t> table;  // count x stride bytes
  std::vector<uint8_t> words;  // extra x 4 bytes
};

struct CarbinPart {
  // Offset (x, y, z), then the half-extents as minimum and maximum.
  std::array<float, 9> bounds{};
  std::array<uint8_t, 28> unknown1{};  // mass, centre of mass, damage fields: not decoded
  std::vector<uint8_t> records;        // u32 count + count x 16 bytes (glass parts)
  std::array<uint8_t, 4> unknown2{};
  std::vector<uint8_t> shorts;         // u32 count + count x 2 bytes
  std::array<uint8_t, 12> unknown3{};
  std::string name;
  uint32_t version = 0;                // 3
  CarbinPool lod_pool;
  std::array<uint8_t, 4> unknown4{};
  std::vector<CarbinSubsection> subsections;
  std::array<uint8_t, 4> unknown5{};
  CarbinPool lod0_pool;
  uint32_t tail_word = 0;              // 1
  uint8_t damage_flag = 0;             // 1 when the part deforms
  std::vector<CarbinDamageBlock> damage;  // LOD, then LOD0
  bool empty_box() const;  // bounds of +/-FLT_MAX: a part with no geometry
};

struct Carbin {
  uint32_t kind = 0;          // first word: 5 exterior, LOD0, cockpit; 1 calipers, rotors, brakes
  std::vector<uint8_t> head;  // every byte before the part count (draw-list block included)
  size_t head_size = 0;       // head.size()
  // The draw-list block: per LOD k (LOD k + 1 in an exterior model with
  // five lists), the sub-sections drawn after the opaque ones and the opaque
  // ones; each entry is part index x 65536 + sub-section index.
  uint32_t draw_list_lods = 0;
  std::vector<std::vector<uint32_t>> transparent_lists, opaque_lists;
  std::vector<CarbinPart> parts;  // empty for a model with no parts
  std::vector<uint8_t> footer;    // 24 bytes: 1, 0, 1, 0, 1, 0
  size_t footer_size = 0;
};

// Offsets of the part count found by the spec's draw-list scan from the
// file's start (every 4-byte offset). A valid model has exactly one.
std::vector<size_t> carbin_scan_part_count(std::span<const uint8_t> data);

// Parses a carbin. Returns false with a reason on any layout mismatch, and
// when the draw-list scan finds no or several positions.
bool parse_carbin(std::span<const uint8_t> data, Carbin& out, std::string* error);

// Writes the model back; an unchanged parse gives the original bytes.
std::vector<uint8_t> encode_carbin(const Carbin& carbin);

// Checks every index of every sub-section against its pool (restart values
// excepted). Returns the number of out-of-range indices.
size_t carbin_bad_indices(const Carbin& carbin);

// Whether the draw lists name every sub-section exactly once, each in the
// list of its own LOD.
bool carbin_draw_lists_cover(const Carbin& carbin);

// Whether a sub-section is in a transparent list (glass, lamp covers).
// Every (part, sub-section) not named by a transparent list is opaque.
std::vector<std::vector<bool>> carbin_transparent_flags(const Carbin& carbin);

// Car-space position of a vertex of `part` (metres; +X right, +Y up,
// +Z rearward).
std::array<float, 3> carbin_position(const CarbinPart& part, const CarbinVertex& v);
// Unit normal from the vertex's tangent frame (the frame's +X axis).
std::array<float, 3> carbin_normal(const CarbinVertex& v);
// Atlas UV of a vertex after the sub-section's transform (set 0 or 1).
std::array<float, 2> carbin_uv(const CarbinSubsection& s, const CarbinVertex& v, int set = 0);
// The sub-section as a triangle list (strips expanded, restarts honoured,
// degenerate triangles dropped). With A, B, C in the returned order,
// (B - A) x (C - A) points out of the surface.
std::vector<uint32_t> carbin_triangle_list(const CarbinSubsection& s);

}  // namespace fh1
