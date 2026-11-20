// FH1 Colorado render models (`coloradoout.NNNNN.rmb.bin` in
// media/tracks/colorado/bin.zip).
//
// Big-endian and packed (no alignment after the first name). A header with
// a box and an object count, then one mesh object per vertex format, each
// holding the GPU vertex buffer and its submeshes (one index list and one
// material each). Material and shader records follow the last object
// (parse_render_materials).
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md);
// WORLD-MATERIALS (docs/formats/WORLD_MATERIALS.md) for the materials, the
// vertex layout and the texture coordinates' uvOffsetScale.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fh1 {

enum class RmbPrimitive : uint32_t { kTriangleList = 4, kTriangleStrip = 6 };

struct RmbSubmesh {
  std::string material;           // material name
  uint32_t primitive = 0;         // RmbPrimitive
  uint32_t material_index = 0;    // probable: 0..n-1 over the model's submeshes
  std::array<float, 4> colour{};  // 1, 1, 1, 1 in every member seen
  std::array<float, 4> sphere{};  // centre and radius (probable)
  uint32_t index_size = 2;        // 2 or 4 bytes
  std::vector<uint32_t> indices;  // widened; a 16-bit strip restarts at 0xFFFF
  uint32_t trailer = 0;           // meaning unknown
};

struct RmbObject {
  std::array<float, 3> centre{};       // object box centre (probable)
  std::array<float, 3> half_extent{};  // (probable)
  std::string name;                    // node name
  uint32_t vertex_format = 0;          // 3 in every member; meaning unknown
  uint32_t vertex_count = 0;
  uint32_t stride = 0;                 // bytes per vertex; the position is bytes 0-11
  std::vector<uint8_t> vertices;       // vertex_count * stride raw (big-endian) bytes
  std::vector<RmbSubmesh> submeshes;

  // Position of vertex `i`: three big-endian f32 at the start of the vertex.
  std::array<float, 3> position(uint32_t i) const;
};

// One material record (WORLD-MATERIALS, "Material records").
struct RmbMaterial {
  uint32_t effect = 0;  // index into RenderModel::effects
  std::vector<std::array<float, 4>> vertex_constants, pixel_constants;
  std::vector<int32_t> slots;  // per texture slot: model texture list entry, -2 none, -1 run time
};

// Where each vertex element sits (byte offsets, -1 absent).
struct RmbVertexLayout {
  int normal = -1, tangent = -1, colour = -1;
  std::array<int, 3> texcoord{-1, -1, -1};
  bool known = false;  // a layout of the stride table or its exceptions
  // The last texture coordinate present: the lightmap and AO coordinate of
  // _LM and _AO effects (probable); -1 when there is none.
  int last_texcoord() const {
    for (int i = 2; i >= 0; --i)
      if (texcoord[size_t(i)] >= 0) return i;
    return -1;
  }
};

struct RenderModel {
  uint32_t kind = 0;                 // 6
  std::array<float, 3> box_min{}, box_max{};
  std::array<float, 16> matrix{};    // identity in every member
  std::array<uint32_t, 4> header_words{};  // 4, 4, 0, 1; meaning unknown
  std::vector<RmbObject> objects;
  size_t geometry_end = 0;           // offset just past the last index list's trailer
  // parse_render_materials:
  std::vector<RmbMaterial> materials;
  std::vector<std::string> effects;  // effect names, upper case: H_ROAD_DIFF3_NORM1_NOISE_AO_LM
  // Per submesh in file order (object by object): offset u, v, scale u, v;
  // empty when the model holds no uvOffsetScale writes.
  std::vector<std::array<float, 4>> uv_offset_scale;
};

// Parses the geometry of a render model. Returns false with a reason on any
// layout mismatch (a read past the end, an unexpected tag, an index size
// other than 2 or 4, a primitive other than a list or strip, or an index
// outside the object's vertices).
bool parse_render_model(std::span<const uint8_t> data, RenderModel& out, std::string* error);

// World-space models hold world positions (terrain, roads, buildings);
// local-space models (props) sit around the origin and are drawn per
// instance. The spec gives no rule; this one is ours: a model is local when
// its header box lies within kRmbLocalRadius of the origin in X and Z. Over
// the disc 200 m splits the models 11,898 / 2,393 as the spec counts them
// (100 m and 400 m do not).
constexpr float kRmbLocalRadius = 200.0f;
bool rmb_is_world_space(const RenderModel& model);

// Reads the materials, the effect path list and the uvOffsetScale writes of
// the command section after the geometry (parse_render_model first). The
// writes are PM4 type-0 writes of 16 or 32 dwords to vertex constant c0
// (header 0x000F4000 or 0x001F4000) whose second constant is (1, 1, 1, 1);
// the first one per submesh is taken.
bool parse_render_materials(std::span<const uint8_t> data, RenderModel& model, std::string* error);

// The vertex layout of an effect (upper-case name) at a stride: the spec's
// stride table and its exceptions by effect name. Position is bytes 0-11.
RmbVertexLayout rmb_vertex_layout(const std::string& effect, uint32_t stride);

// Element decoders. NORMAL and TANGENT: a big-endian u32, x bits 0-9, y
// 10-19, z 20-29, each a signed 10-bit value / 511. TEXCOORD: two
// big-endian u16, u first, as a fraction of 65,535 (before uvOffsetScale).
// COLOR: four bytes in memory order A, R, G, B (probable); returned as
// R, G, B, A.
std::array<float, 3> rmb_normal(const uint8_t* p);
std::array<float, 2> rmb_texcoord(const uint8_t* p);
std::array<uint8_t, 4> rmb_colour(const uint8_t* p);

// The submesh as a triangle list: a strip is expanded (restart at 0xFFFF
// for 16-bit lists, all ones for 32-bit lists), degenerate triangles are
// dropped and every second strip triangle is flipped to keep the winding.
std::vector<uint32_t> rmb_triangle_list(const RmbSubmesh& submesh);

}  // namespace fh1
