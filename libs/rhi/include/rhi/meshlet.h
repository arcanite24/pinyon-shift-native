// GPU-Driven Meshlet / Cluster builder and Visibility Buffer GPU data structures.
// Subdivides indexed triangle meshes into 64-vertex / <=124-triangle clusters
// with tight bounding spheres and backface normal cones for 2-phase GPU culling
// and 64-bit Visibility Buffer (R32G32_UINT) rendering across Vulkan/DX12/Metal.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace shift::rhi {

inline constexpr uint32_t kMaxMeshletVertices = 64u;
inline constexpr uint32_t kMaxMeshletTriangles = 124u;

// Material classes for 8x8 tile classification and specialized compute shading.
enum class VisMaterialClass : uint32_t {
  Terrain = 0,
  Road = 1,
  Foliage = 2,
  CarPaint = 3,
  MetalPlastic = 4,
  Count = 5,
};

inline constexpr uint32_t kVisMaterialClassCount =
    static_cast<uint32_t>(VisMaterialClass::Count);

// 48-byte interleaved vertex layout for Visibility Buffer raster + compute attribute pull.
struct alignas(16) VisVertexGpu {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float u = 0.0f;
  float normal[3] = {0.0f, 1.0f, 0.0f};
  float v = 0.0f;
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(VisVertexGpu) == 48, "VisVertexGpu must be 48 bytes");

// 64-byte GPU meshlet descriptor with bounding sphere & backface normal cone.
struct alignas(16) MeshletGpu {
  float bound_center[3] = {0.0f, 0.0f, 0.0f};
  float bound_radius = 0.0f;
  float cone_apex[3] = {0.0f, 0.0f, 0.0f};
  float cone_cutoff = 1.0f;  // cos(angle + pi/2); >= 1.0 disables backface cone cull
  float cone_axis[3] = {0.0f, 1.0f, 0.0f};
  uint32_t index_offset = 0;  // first index in global index buffer
  uint32_t vertex_offset = 0; // base vertex offset in global vertex buffer
  uint32_t triangle_count = 0;
  uint32_t vertex_count = 0;
  uint32_t material_id = 0;
};
static_assert(sizeof(MeshletGpu) == 64, "MeshletGpu must be 64 bytes");

// 144-byte per-instance GPU transform & meshlet range descriptor.
struct alignas(16) MeshletInstanceGpu {
  float model[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
  };
  float prev_model[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
  };
  uint32_t first_meshlet = 0;
  uint32_t meshlet_count = 0;
  uint32_t material_class = 0;
  uint32_t material_id = 0;
};
static_assert(sizeof(MeshletInstanceGpu) == 144, "MeshletInstanceGpu must be 144 bytes");

// 48-byte PBR material entry read by specialized compute material shaders.
struct alignas(16) VisMaterialGpu {
  float base_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  float emissive[3] = {0.0f, 0.0f, 0.0f};
  float roughness = 0.5f;
  float metallic = 0.0f;
  float clearcoat = 0.0f;
  uint32_t material_class = 0;
  uint32_t albedo_tex_slot = 0xFFFFFFFFu;
};
static_assert(sizeof(VisMaterialGpu) == 48, "VisMaterialGpu must be 48 bytes");

// 16-byte visible draw record emitted by vis_cull.comp and indexed by gl_InstanceIndex / SV_InstanceID.
struct alignas(16) VisibleMeshletDrawGpu {
  uint32_t instance_id = 0;
  uint32_t meshlet_id = 0;
  uint32_t material_class = 0;
  uint32_t material_id = 0;
};
static_assert(sizeof(VisibleMeshletDrawGpu) == 16, "VisibleMeshletDrawGpu must be 16 bytes");

// 20-byte indexed indirect draw command matching VkDrawIndexedIndirectCommand &
// D3D12_DRAW_INDEXED_ARGUMENTS bit-for-bit.
struct DrawIndexedIndirectCmd {
  uint32_t index_count = 0;
  uint32_t instance_count = 0;
  uint32_t first_index = 0;
  int32_t vertex_offset = 0;
  uint32_t first_instance = 0;
};
static_assert(sizeof(DrawIndexedIndirectCmd) == 20, "DrawIndexedIndirectCmd must be 20 bytes");

struct MeshletBuildOutput {
  std::vector<MeshletGpu> meshlets;
  std::vector<uint32_t> reordered_indices;
};

// Subdivides an indexed triangle list into spatial 64-vertex / <=124-triangle
// meshlets with tight bounding spheres and backface normal cones.
MeshletBuildOutput build_meshlets(std::span<const VisVertexGpu> vertices,
                                  std::span<const uint32_t> indices,
                                  uint32_t material_id = 0,
                                  uint32_t global_vertex_offset = 0,
                                  uint32_t global_index_offset = 0,
                                  uint32_t max_vertices = kMaxMeshletVertices,
                                  uint32_t max_triangles = kMaxMeshletTriangles);

// Extracts 6 normalized Gribb-Hartmann frustum planes (ax + by + cz + d >= 0 inside)
// from a column-major 4x4 view-projection matrix (Vulkan/DX12 [0,1] depth convention).
void extract_frustum_planes(const float view_proj[16], float out_planes[6][4]) noexcept;

}  // namespace shift::rhi
