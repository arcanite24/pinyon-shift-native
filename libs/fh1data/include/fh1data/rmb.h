// FH1 Colorado render models (`coloradoout.NNNNN.rmb.bin` in
// media/tracks/colorado/bin.zip).
//
// Big-endian and packed (no alignment after the first name). A header with
// a box and an object count, then one mesh object per vertex format, each
// holding the GPU vertex buffer and its submeshes (one index list and one
// material each). Material and shader records follow the last object; they
// are not decoded, the reader stops at the end of the geometry.
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md).
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

struct RenderModel {
  uint32_t kind = 0;                 // 6
  std::array<float, 3> box_min{}, box_max{};
  std::array<float, 16> matrix{};    // identity in every member
  std::array<uint32_t, 4> header_words{};  // 4, 4, 0, 1; meaning unknown
  std::vector<RmbObject> objects;
  size_t geometry_end = 0;           // offset just past the last index list's trailer
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

// The submesh as a triangle list: a strip is expanded (restart at 0xFFFF
// for 16-bit lists, all ones for 32-bit lists), degenerate triangles are
// dropped and every second strip triangle is flipped to keep the winding.
std::vector<uint32_t> rmb_triangle_list(const RmbSubmesh& submesh);

}  // namespace fh1
