// FH1 `.carbin` car models (media/cars/<MediaName>.zip).
//
// Big-endian. A header (not decoded), a part count (not always 4-byte
// aligned), the parts, then a footer (not decoded). Each part has a box
// (offset, half-extents), a name, a LOD vertex pool (LOD1-5), sub-sections
// (one per material and LOD, each an index buffer with a UV transform), a
// LOD0 vertex pool and a damage tail. `<car>.carbin` fills only the LOD
// pools, `_lod0`, `_cockpit`, caliper and rotor carbins only the LOD0 pools.
// `stripped_*` twins use a sub-section layout this reader does not decode.
// Spec: site/findings/files.toml (carbin layout, carbin vertex, carbin
// sub-sections, carbin damage tail) and tools/fh1_carbin.py in horizon1-recomp.
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
// (/ 32767, turns +X to the normal), four bytes of baked lighting (the
// first behaves as ambient occlusion, about 100 when open to the sky).
struct CarbinVertex {
  std::array<int16_t, 4> position;
  std::array<uint16_t, 2> uv0;
  std::array<uint16_t, 2> uv1;
  std::array<int16_t, 4> frame;
  std::array<uint8_t, 4> lighting;
};

struct CarbinPool {
  int32_t count = 0;          // signed: a few pools store a negative count (no vertices)
  uint32_t stride = 0;        // 28 for the decoded layout
  uint32_t bone_weights = 0;
  std::optional<uint32_t> part_id;  // present when bone_weights != 0
  std::vector<CarbinVertex> vertices;  // decoded when stride == 28
  size_t raw_bytes = 0;
};

enum class CarbinIndexType : uint32_t { kTriangleList = 4, kTriangleStrip = 6 };

struct CarbinSubsection {
  // (U offset, U scale, V offset, V scale) for UV0, then the same for UV1.
  std::array<float, 8> uv_transform{};
  std::string material;       // a car shader technique, e.g. "body", "window"
  int32_t lod = 0;            // 0 indexes the LOD0 pool, 1-5 the LOD pool
  uint32_t index_type = 0;    // CarbinIndexType
  std::array<uint32_t, 2> version_words{};
  uint32_t index_size = 2;    // 2 or 4 bytes
  std::vector<uint32_t> indices;
};

struct CarbinPart {
  // Offset (x, y, z), then the half-extents as minimum and maximum.
  std::array<float, 9> bounds{};
  std::string name;
  uint32_t version = 0;
  CarbinPool lod_pool;
  std::vector<CarbinSubsection> subsections;
  CarbinPool lod0_pool;
  uint32_t tail_word = 0;
  uint8_t damage_flag = 0;
  bool empty_box() const;  // bounds of +/-FLT_MAX: a part with no geometry
};

struct Carbin {
  size_t head_size = 0;    // bytes before the part count
  size_t footer_size = 0;  // bytes after the last part
  std::vector<CarbinPart> parts;  // empty for a header-only model
};

// Parses a carbin. Returns false with a reason on any layout mismatch.
bool parse_carbin(std::span<const uint8_t> data, Carbin& out, std::string* error);

// Checks every index of every sub-section against its pool (restart values
// excepted). Returns the number of out-of-range indices.
size_t carbin_bad_indices(const Carbin& carbin);

// Car-space position of a vertex of `part` (metres; +X right, +Y up,
// +Z rearward).
std::array<float, 3> carbin_position(const CarbinPart& part, const CarbinVertex& v);
// Unit normal from the vertex's tangent frame (the frame's +X axis).
std::array<float, 3> carbin_normal(const CarbinVertex& v);
// Atlas UV of a vertex after the sub-section's transform (set 0 or 1).
std::array<float, 2> carbin_uv(const CarbinSubsection& s, const CarbinVertex& v, int set = 0);
// The sub-section as a triangle list (strips expanded, restarts honoured,
// degenerate triangles dropped). Winding follows the strip's parity.
std::vector<uint32_t> carbin_triangle_list(const CarbinSubsection& s);

}  // namespace fh1
