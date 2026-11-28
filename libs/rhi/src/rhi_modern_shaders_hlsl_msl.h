// Multi-target HLSL SM 6.6 (DirectX 12 via DXC) and MSL 3.1 (Apple Metal 3)
// shaders matching libs/rhi/shaders/* SPIR-V pipelines bit-for-bit.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "rhi/rhi.h"

namespace shift::rhi::shaders {

inline ShaderModuleDesc make_shader(BackendKind backend,
                                    ShaderStage stage,
                                    std::span<const uint32_t> spv,
                                    std::string_view hlsl,
                                    std::string_view msl,
                                    const char* entry = "main") noexcept {
  ShaderModuleDesc sm{};
  sm.stage = stage;
  sm.entry_point = entry;
  if (backend == BackendKind::Vulkan) {
    sm.spirv = spv;
  } else if (backend == BackendKind::D3D12) {
    sm.dxil = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(hlsl.data()), hlsl.size());
  } else if (backend == BackendKind::Metal) {
    sm.msl = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(msl.data()), msl.size());
  }
  return sm;
}

// =============================================================================
// 1. Clustered Forward+ 3D Froxel Light Culling (light_cluster.comp)
// =============================================================================
inline constexpr std::string_view kLightClusterCompHlsl = R"(
cbuffer CullParams : register(b0, space0) {
  column_major float4x4 view;
  column_major float4x4 inv_proj;
  float z_near;
  float z_far;
  uint light_count;
  uint grid_x;
  uint grid_y;
  uint grid_z;
  uint max_lights_per_cluster;
  uint pad;
};

ByteAddressBuffer light_buf : register(t1, space0);
RWByteAddressBuffer cell_buf : register(u2, space0);
RWByteAddressBuffer index_buf : register(u3, space0);

float3 screen_to_view(float2 uv, float ndc_z) {
  float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndc_z, 1.0);
  float4 v = mul(inv_proj, clip);
  return v.xyz / max(abs(v.w), 1e-6);
}

float3 ray_to_z_plane(float3 ray_dir, float z_plane) {
  float t = z_plane / (abs(ray_dir.z) > 1e-6 ? ray_dir.z : -1e-6);
  return ray_dir * t;
}

bool sphere_aabb_overlap(float3 center, float radius, float3 bmin, float3 bmax) {
  float3 closest = clamp(center, bmin, bmax);
  float3 d = center - closest;
  return dot(d, d) <= radius * radius;
}

[numthreads(8, 4, 2)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= grid_x || id.y >= grid_y || id.z >= grid_z) return;

  uint cluster_idx = id.x + id.y * grid_x + id.z * (grid_x * grid_y);
  uint base_offset = cluster_idx * max_lights_per_cluster;

  float2 uv_min = float2(id.xy) / float2(grid_x, grid_y);
  float2 uv_max = float2(id.xy + uint2(1, 1)) / float2(grid_x, grid_y);

  float3 c00 = screen_to_view(float2(uv_min.x, uv_min.y), 0.5);
  float3 c10 = screen_to_view(float2(uv_max.x, uv_min.y), 0.5);
  float3 c01 = screen_to_view(float2(uv_min.x, uv_max.y), 0.5);
  float3 c11 = screen_to_view(float2(uv_max.x, uv_max.y), 0.5);

  float ratio = z_far / max(z_near, 1e-4);
  float slice_near = -z_near * pow(ratio, float(id.z) / float(grid_z));
  float slice_far  = -z_near * pow(ratio, float(id.z + 1u) / float(grid_z));

  float3 p0 = ray_to_z_plane(c00, slice_near);
  float3 p1 = ray_to_z_plane(c10, slice_near);
  float3 p2 = ray_to_z_plane(c01, slice_near);
  float3 p3 = ray_to_z_plane(c11, slice_near);
  float3 p4 = ray_to_z_plane(c00, slice_far);
  float3 p5 = ray_to_z_plane(c10, slice_far);
  float3 p6 = ray_to_z_plane(c01, slice_far);
  float3 p7 = ray_to_z_plane(c11, slice_far);

  float3 aabb_min = min(min(min(p0, p1), min(p2, p3)), min(min(p4, p5), min(p6, p7)));
  float3 aabb_max = max(max(max(p0, p1), max(p2, p3)), max(max(p4, p5), max(p6, p7)));

  uint count = 0u;
  uint n_lights = min(light_count, 256u);
  for (uint i = 0u; i < n_lights; ++i) {
    float4 pos_radius = asfloat(light_buf.Load4(i * 64u));
    float3 lpos_view = mul(view, float4(pos_radius.xyz, 1.0)).xyz;
    if (sphere_aabb_overlap(lpos_view, pos_radius.w, aabb_min, aabb_max)) {
      if (count < max_lights_per_cluster) {
        index_buf.Store((base_offset + count) * 4u, i);
        ++count;
      }
    }
  }
  cell_buf.Store2(cluster_idx * 8u, uint2(base_offset, count));
}
)";

inline constexpr std::string_view kLightClusterCompMsl = R"(
// shift_local_size(8, 4, 2)
#include <metal_stdlib>
using namespace metal;
struct ClusterLight { float4 pos_radius; float4 color_intensity; float4 dir_cos_outer; float cos_inner; uint type; uint pad0; uint pad1; };
struct ClusterCell { uint offset; uint count; };
struct CullParams { float4x4 view; float4x4 inv_proj; float z_near; float z_far; uint light_count; uint grid_x; uint grid_y; uint grid_z; uint max_lights_per_cluster; uint pad; };
static float3 screen_to_view(constant CullParams& u, float2 uv, float ndc_z) {
  float4 v = u.inv_proj * float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, ndc_z, 1.0f);
  return v.xyz / max(abs(v.w), 1e-6f);
}
static float3 ray_to_z(float3 d, float z) { return d * (z / (abs(d.z) > 1e-6f ? d.z : -1e-6f)); }
kernel void main0(constant CullParams& u [[buffer(0)]],
                  device const ClusterLight* lights [[buffer(1)]],
                  device ClusterCell* cells [[buffer(2)]],
                  device uint* light_indices [[buffer(3)]],
                  uint3 id [[thread_position_in_grid]]) {
  if (id.x >= u.grid_x || id.y >= u.grid_y || id.z >= u.grid_z) return;
  uint idx = id.x + id.y * u.grid_x + id.z * (u.grid_x * u.grid_y);
  uint base = idx * u.max_lights_per_cluster;
  float2 uv0 = float2(id.xy) / float2(u.grid_x, u.grid_y);
  float2 uv1 = float2(id.xy + uint2(1)) / float2(u.grid_x, u.grid_y);
  float ratio = u.z_far / max(u.z_near, 1e-4f);
  float zn = -u.z_near * pow(ratio, float(id.z) / float(u.grid_z));
  float zf = -u.z_near * pow(ratio, float(id.z + 1u) / float(u.grid_z));
  float3 c00 = screen_to_view(u, uv0, 0.5f), c10 = screen_to_view(u, float2(uv1.x, uv0.y), 0.5f);
  float3 c01 = screen_to_view(u, float2(uv0.x, uv1.y), 0.5f), c11 = screen_to_view(u, uv1, 0.5f);
  float3 p0=ray_to_z(c00,zn), p1=ray_to_z(c10,zn), p2=ray_to_z(c01,zn), p3=ray_to_z(c11,zn);
  float3 p4=ray_to_z(c00,zf), p5=ray_to_z(c10,zf), p6=ray_to_z(c01,zf), p7=ray_to_z(c11,zf);
  float3 bmin = min(min(min(p0,p1),min(p2,p3)),min(min(p4,p5),min(p6,p7)));
  float3 bmax = max(max(max(p0,p1),max(p2,p3)),max(max(p4,p5),max(p6,p7)));
  uint cnt = 0;
  for (uint i = 0; i < min(u.light_count, 256u); ++i) {
    float3 lp = (u.view * float4(lights[i].pos_radius.xyz, 1.0f)).xyz;
    float3 d = lp - clamp(lp, bmin, bmax);
    if (dot(d, d) <= lights[i].pos_radius.w * lights[i].pos_radius.w && cnt < u.max_lights_per_cluster) {
      light_indices[base + cnt++] = i;
    }
  }
  cells[idx] = {base, cnt};
}
)";

// =============================================================================
// 2. GPU Meshlet Frustum + Backface Normal Cone Culling (vis_cull.comp)
// =============================================================================
inline constexpr std::string_view kVisCullCompHlsl = R"(
cbuffer CullParams : register(b0, space0) {
  column_major float4x4 view_proj;
  float4 frustum_planes[6];
  float4 camera_pos_pad;
  uint work_item_count;
  uint max_draws;
  uint enable_cone_cull;
  uint pad;
};

ByteAddressBuffer instance_buf : register(t1, space0);
ByteAddressBuffer meshlet_buf : register(t2, space0);
ByteAddressBuffer work_item_buf : register(t3, space0);
RWByteAddressBuffer draw_cmd_buf : register(u4, space0);
RWByteAddressBuffer visible_draw_buf : register(u5, space0);
RWByteAddressBuffer stats_buf : register(u6, space0);

[numthreads(64, 1, 1)]
void main(uint3 gtid : SV_DispatchThreadID) {
  uint tid = gtid.x;
  if (tid >= work_item_count) return;
  uint dummy;
  stats_buf.InterlockedAdd(12u, 1u, dummy);

  uint2 item = work_item_buf.Load2(tid * 8u);
  uint inst_off = item.x * 144u;
  float4 m0 = asfloat(instance_buf.Load4(inst_off + 0u));
  float4 m1 = asfloat(instance_buf.Load4(inst_off + 16u));
  float4 m2 = asfloat(instance_buf.Load4(inst_off + 32u));
  float4 m3 = asfloat(instance_buf.Load4(inst_off + 48u));
  uint4 inst_meta = instance_buf.Load4(inst_off + 128u);

  uint m_off = item.y * 64u;
  float4 bound_sphere = asfloat(meshlet_buf.Load4(m_off + 0u));
  float4 cone_apex_cut = asfloat(meshlet_buf.Load4(m_off + 16u));
  uint4 m_r2 = meshlet_buf.Load4(m_off + 32u);
  uint4 m_r3 = meshlet_buf.Load4(m_off + 48u);
  float3 cone_axis = asfloat(m_r2.xyz);
  uint index_offset = m_r2.w;
  uint vertex_offset = m_r3.x;
  uint triangle_count = m_r3.y;

  float3 world_center = (m0 * bound_sphere.x + m1 * bound_sphere.y + m2 * bound_sphere.z + m3).xyz;
  float max_scale = max(length(m0.xyz), max(length(m1.xyz), length(m2.xyz)));
  float world_radius = bound_sphere.w * max_scale;

  for (uint p = 0u; p < 6u; ++p) {
    float4 pl = frustum_planes[p];
    if (dot(pl.xyz, world_center) + pl.w < -world_radius) {
      stats_buf.InterlockedAdd(4u, 1u, dummy);
      return;
    }
  }

  if (enable_cone_cull != 0u && cone_apex_cut.w < 0.999) {
    float3 world_axis = normalize(m0.xyz * cone_axis.x + m1.xyz * cone_axis.y + m2.xyz * cone_axis.z);
    float3 view_vec = world_center - camera_pos_pad.xyz;
    float dist = length(view_vec);
    if (dist > world_radius && dot(view_vec, world_axis) >= cone_apex_cut.w * dist + world_radius) {
      stats_buf.InterlockedAdd(8u, 1u, dummy);
      return;
    }
  }

  uint slot;
  stats_buf.InterlockedAdd(0u, 1u, slot);
  if (slot >= max_draws) return;

  uint cmd_off = slot * 20u;
  draw_cmd_buf.Store4(cmd_off + 0u, uint4(triangle_count * 3u, 1u, index_offset, vertex_offset));
  draw_cmd_buf.Store(cmd_off + 16u, slot);

  visible_draw_buf.Store4(slot * 16u, uint4(item.x, item.y, inst_meta.z, inst_meta.w));
}
)";

inline constexpr std::string_view kVisCullCompMsl = R"(
// shift_local_size(64, 1, 1)
#include <metal_stdlib>
using namespace metal;
struct MeshletInstance { float4x4 model; float4x4 prev_model; uint first_meshlet; uint meshlet_count; uint material_class; uint material_id; };
struct Meshlet { float4 bound_sphere; float4 cone_apex_cut; packed_float3 cone_axis; uint index_offset; uint vertex_offset; uint triangle_count; uint vertex_count; uint material_id; };
struct DrawCmd { uint index_count; uint instance_count; uint first_index; int vertex_offset; uint first_instance; };
struct VisibleDraw { uint instance_id; uint meshlet_id; uint material_class; uint material_id; };
struct CullParams { float4x4 view_proj; float4 frustum_planes[6]; float4 camera_pos_pad; uint work_item_count; uint max_draws; uint enable_cone_cull; uint pad; };
kernel void main0(constant CullParams& u [[buffer(0)]],
                  device const MeshletInstance* instances [[buffer(1)]],
                  device const Meshlet* meshlets [[buffer(2)]],
                  device const uint2* work_items [[buffer(3)]],
                  device DrawCmd* cmds [[buffer(4)]],
                  device VisibleDraw* vis_draws [[buffer(5)]],
                  device atomic_uint* stats [[buffer(6)]],
                  uint tid [[thread_position_in_grid]]) {
  if (tid >= u.work_item_count) return;
  atomic_fetch_add_explicit(&stats[3], 1u, memory_order_relaxed);
  uint2 item = work_items[tid];
  MeshletInstance inst = instances[item.x];
  Meshlet m = meshlets[item.y];
  float3 wc = (inst.model * float4(m.bound_sphere.xyz, 1.0f)).xyz;
  float wr = m.bound_sphere.w * max(length(inst.model[0].xyz), max(length(inst.model[1].xyz), length(inst.model[2].xyz)));
  for (uint p = 0; p < 6; ++p) {
    if (dot(u.frustum_planes[p].xyz, wc) + u.frustum_planes[p].w < -wr) {
      atomic_fetch_add_explicit(&stats[1], 1u, memory_order_relaxed);
      return;
    }
  }
  if (u.enable_cone_cull != 0u && m.cone_apex_cut.w < 0.999f) {
    float3 c_axis = float3(m.cone_axis[0], m.cone_axis[1], m.cone_axis[2]);
    float3 world_axis = normalize(inst.model[0].xyz * c_axis.x + inst.model[1].xyz * c_axis.y + inst.model[2].xyz * c_axis.z);
    float3 view_vec = wc - u.camera_pos_pad.xyz;
    float dist = length(view_vec);
    if (dist > wr && dot(view_vec, world_axis) >= m.cone_apex_cut.w * dist + wr) {
      atomic_fetch_add_explicit(&stats[2], 1u, memory_order_relaxed);
      return;
    }
  }
  uint slot = atomic_fetch_add_explicit(&stats[0], 1u, memory_order_relaxed);
  if (slot >= u.max_draws) return;
  cmds[slot] = {m.triangle_count * 3u, 1u, m.index_offset, int(m.vertex_offset), slot};
  vis_draws[slot] = {item.x, item.y, inst.material_class, inst.material_id};
}
)";

// =============================================================================
// 3. 64-bit Visibility Buffer Rasterizer (vis_raster.vert / vis_raster.frag)
// =============================================================================
inline constexpr std::string_view kVisRasterVertHlsl = R"(
cbuffer FrameParams : register(b0, space0) {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
};

ByteAddressBuffer instance_buf : register(t1, space0);
ByteAddressBuffer visible_draw_buf : register(t2, space0);

struct VsIn {
  float3 pos : TEXCOORD0;
  uint draw_id : TEXCOORD1;
};

struct VsOut {
  float4 pos : SV_Position;
  nointerpolation uint2 vis_meta : TEXCOORD0;
};

VsOut main(VsIn input) {
  uint4 vd = visible_draw_buf.Load4(input.draw_id * 16u);
  uint inst_off = vd.x * 144u;
  float4 m0 = asfloat(instance_buf.Load4(inst_off + 0u));
  float4 m1 = asfloat(instance_buf.Load4(inst_off + 16u));
  float4 m2 = asfloat(instance_buf.Load4(inst_off + 32u));
  float4 m3 = asfloat(instance_buf.Load4(inst_off + 48u));

  float4 world_pos = m0 * input.pos.x + m1 * input.pos.y + m2 * input.pos.z + m3;

  VsOut o;
  o.pos = mul(view_proj, world_pos);
  o.vis_meta = uint2(input.draw_id + 1u, (vd.z << 16u) | (vd.y & 0xFFFFu));
  return o;
}
)";

inline constexpr std::string_view kVisRasterFragHlsl = R"(
struct PsIn {
  float4 pos : SV_Position;
  nointerpolation uint2 vis_meta : TEXCOORD0;
};

uint2 main(PsIn input, uint prim_id : SV_PrimitiveID) : SV_Target0 {
  return uint2((input.vis_meta.x << 8u) | (prim_id & 0xFFu), input.vis_meta.y);
}
)";

inline constexpr std::string_view kVisRasterVertMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct FrameParams { float4x4 view_proj; float4x4 prev_view_proj; };
struct MeshletInstance { float4x4 model; float4x4 prev_model; uint first_meshlet; uint meshlet_count; uint material_class; uint material_id; };
struct VisibleDraw { uint instance_id; uint meshlet_id; uint material_class; uint material_id; };
struct VsIn { float3 pos [[attribute(0)]]; uint draw_id [[attribute(1)]]; };
struct VsOut { float4 pos [[position]]; uint2 vis_meta [[flat]]; };
vertex VsOut main0(VsIn in [[stage_in]],
                   constant FrameParams& u [[buffer(0)]],
                   device const MeshletInstance* instances [[buffer(1)]],
                   device const VisibleDraw* vis_draws [[buffer(2)]]) {
  VisibleDraw vd = vis_draws[in.draw_id];
  VsOut o;
  o.pos = u.view_proj * (instances[vd.instance_id].model * float4(in.pos, 1.0f));
  o.vis_meta = uint2(in.draw_id + 1u, (vd.material_class << 16u) | (vd.meshlet_id & 0xFFFFu));
  return o;
}
)";

inline constexpr std::string_view kVisRasterFragMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct PsIn { float4 pos [[position]]; uint2 vis_meta [[flat]]; };
fragment uint2 main0(PsIn in [[stage_in]], uint prim_id [[primitive_id]]) {
  return uint2((in.vis_meta.x << 8u) | (prim_id & 0xFFu), in.vis_meta.y);
}
)";

// =============================================================================
// 4. 8x8 LDS Tile Material Classifier (vis_classify.comp)
// =============================================================================
inline constexpr std::string_view kVisClassifyCompHlsl = R"(
cbuffer ClassifyParams : register(b0, space0) {
  uint width;
  uint height;
  uint tiles_x;
  uint tiles_y;
  uint max_tiles_per_class;
  uint material_class_count;
  uint pad0;
  uint pad1;
};

RWTexture2D<uint2> vis_buf : register(u1, space0);
RWByteAddressBuffer indirect_args : register(u2, space0);
RWByteAddressBuffer tile_lists : register(u3, space0);

groupshared uint s_tile_mask;

[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint gidx : SV_GroupIndex) {
  if (gidx == 0u) {
    s_tile_mask = 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  if (gtid.x < width && gtid.y < height) {
    uint2 vis = vis_buf[gtid.xy];
    if (vis.x != 0u) {
      uint mat_class = vis.y >> 16u;
      if (mat_class < material_class_count) {
        InterlockedOr(s_tile_mask, 1u << mat_class);
      }
    }
  }
  GroupMemoryBarrierWithGroupSync();

  if (gidx == 0u && s_tile_mask != 0u) {
    uint tile_packed = (gid.x & 0xFFFFu) | ((gid.y & 0xFFFFu) << 16u);
    uint mask = s_tile_mask;
    for (uint c = 0u; c < material_class_count; ++c) {
      if ((mask & (1u << c)) != 0u) {
        uint slot;
        indirect_args.InterlockedAdd(c * 12u, 1u, slot);
        if (slot < max_tiles_per_class) {
          tile_lists.Store((c * max_tiles_per_class + slot) * 4u, tile_packed);
        }
      }
    }
  }
}
)";

inline constexpr std::string_view kVisClassifyCompMsl = R"(
// shift_local_size(8, 8, 1)
#include <metal_stdlib>
using namespace metal;
struct ClassifyParams { uint width; uint height; uint tiles_x; uint tiles_y; uint max_tiles_per_class; uint material_class_count; uint pad0; uint pad1; };
kernel void main0(constant ClassifyParams& u [[buffer(0)]],
                  texture2d<uint, access::read> vis_buf [[texture(1)]],
                  device atomic_uint* indirect_args [[buffer(2)]],
                  device uint* tile_lists [[buffer(3)]],
                  uint2 gtid [[thread_position_in_grid]],
                  uint2 gid [[threadgroup_position_in_grid]],
                  uint gidx [[thread_index_in_threadgroup]]) {
  threadgroup atomic_uint s_mask;
  if (gidx == 0u) atomic_store_explicit(&s_mask, 0u, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (gtid.x < u.width && gtid.y < u.height) {
    uint2 v = vis_buf.read(gtid).xy;
    if (v.x != 0u) {
      uint mc = v.y >> 16u;
      if (mc < u.material_class_count) atomic_fetch_or_explicit(&s_mask, 1u << mc, memory_order_relaxed);
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (gidx == 0u) {
    uint mask = atomic_load_explicit(&s_mask, memory_order_relaxed);
    uint packed = (gid.x & 0xFFFFu) | ((gid.y & 0xFFFFu) << 16u);
    for (uint c = 0; c < u.material_class_count; ++c) {
      if (mask & (1u << c)) {
        uint slot = atomic_fetch_add_explicit(&indirect_args[c * 3u], 1u, memory_order_relaxed);
        if (slot < u.max_tiles_per_class) tile_lists[c * u.max_tiles_per_class + slot] = packed;
      }
    }
  }
}
)";

// =============================================================================
// 5. Specialized Compute Material Shader + Clustered Lights (vis_shade.comp)
// =============================================================================
inline constexpr std::string_view kVisShadeCompHlsl = R"(
cbuffer ShadePush : register(b0, space15) {
  uint active_material_class;
  uint max_tiles_per_class;
};

cbuffer ShadeParams : register(b0, space0) {
  column_major float4x4 view;
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  float4 camera_pos_znear;
  float4 sun_dir_zfar;
  float4 sun_color_intensity;
  uint4 viewport_grid;
  uint4 grid_z_pad;
};

RWTexture2D<uint2> vis_buf : register(u1, space0);
ByteAddressBuffer tile_lists : register(t2, space0);
ByteAddressBuffer vertex_buf : register(t3, space0);
ByteAddressBuffer index_buf : register(t4, space0);
ByteAddressBuffer meshlet_buf : register(t5, space0);
ByteAddressBuffer instance_buf : register(t6, space0);
ByteAddressBuffer visible_draw_buf : register(t7, space0);
ByteAddressBuffer material_buf : register(t8, space0);
ByteAddressBuffer light_buf : register(t9, space0);
ByteAddressBuffer cell_buf : register(t10, space0);
ByteAddressBuffer light_idx_buf : register(t11, space0);
RWTexture2D<float4> out_hdr : register(u12, space0);
RWTexture2D<float4> out_gbuf : register(u13, space0);
RWTexture2D<float2> out_velocity : register(u14, space0);

float2 encode_octahedral(float3 n) {
  n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-6);
  if (n.z < 0.0) {
    float2 sgn = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    n.xy = (1.0 - abs(n.yx)) * sgn;
  }
  return n.xy * 0.5 + 0.5;
}

float3 eval_ggx_brdf(float3 N, float3 V, float3 L, float3 albedo, float roughness, float metallic, float clearcoat) {
  float ndotl = max(dot(N, L), 0.0);
  if (ndotl <= 0.0) return float3(0.0, 0.0, 0.0);
  float3 H = normalize(V + L);
  float ndotv = max(dot(N, V), 1e-4);
  float ndoth = max(dot(N, H), 0.0);
  float vdoth = max(dot(V, H), 0.0);

  float a = max(roughness * roughness, 0.04);
  float a2 = a * a;
  float denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
  float D = a2 / (3.14159265 * denom * denom + 1e-6);

  float k = roughness + 1.0;
  k = (k * k) * 0.125;
  float G = (ndotv / (ndotv * (1.0 - k) + k)) * (ndotl / (ndotl * (1.0 - k) + k));

  float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
  float3 F = F0 + (1.0 - F0) * pow(1.0 - vdoth, 5.0);

  float3 spec = (D * G * F) / (4.0 * ndotv * ndotl + 1e-5);
  float3 kd = (1.0 - F) * (1.0 - metallic);
  float3 diff = kd * albedo * (1.0 / 3.14159265);
  float3 res = (diff + spec) * ndotl;

  if (clearcoat > 0.01) {
    float ca = 0.06;
    float ca2 = ca * ca;
    float cden = ndoth * ndoth * (ca2 - 1.0) + 1.0;
    float cD = ca2 / (3.14159265 * cden * cden + 1e-6);
    float cF = 0.04 + 0.96 * pow(1.0 - vdoth, 5.0);
    res += (cD * cF * 0.25 * clearcoat * ndotl).xxx;
  }
  return res;
}

float3 mul_mat4_pt(ByteAddressBuffer buf, uint off, float3 p) {
  float4 c0 = asfloat(buf.Load4(off + 0u));
  float4 c1 = asfloat(buf.Load4(off + 16u));
  float4 c2 = asfloat(buf.Load4(off + 32u));
  float4 c3 = asfloat(buf.Load4(off + 48u));
  return (c0 * p.x + c1 * p.y + c2 * p.z + c3).xyz;
}

float3 mul_mat3_vec(ByteAddressBuffer buf, uint off, float3 v) {
  float3 c0 = asfloat(buf.Load3(off + 0u));
  float3 c1 = asfloat(buf.Load3(off + 16u));
  float3 c2 = asfloat(buf.Load3(off + 32u));
  return c0 * v.x + c1 * v.y + c2 * v.z;
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 ltid : SV_GroupThreadID) {
  uint tile_packed = tile_lists.Load((active_material_class * max_tiles_per_class + gid.x) * 4u);
  uint2 tile_xy = uint2(tile_packed & 0xFFFFu, tile_packed >> 16u);
  uint2 px = tile_xy * 8u + ltid.xy;
  if (px.x >= viewport_grid.x || px.y >= viewport_grid.y) return;

  uint2 vis = vis_buf[px];
  if (vis.x == 0u) return;
  if ((vis.y >> 16u) != active_material_class) return;

  uint draw_id = (vis.x >> 8u) - 1u;
  uint tri_id = vis.x & 0xFFu;

  uint4 vd = visible_draw_buf.Load4(draw_id * 16u);
  uint inst_off = vd.x * 144u;
  uint m_off = vd.y * 64u;
  uint mat_off = vd.w * 48u;

  uint index_offset = meshlet_buf.Load(m_off + 44u);
  uint vertex_offset = meshlet_buf.Load(m_off + 48u);

  uint idx_base = (index_offset + tri_id * 3u) * 4u;
  uint i0 = index_buf.Load(idx_base + 0u) + vertex_offset;
  uint i1 = index_buf.Load(idx_base + 4u) + vertex_offset;
  uint i2 = index_buf.Load(idx_base + 8u) + vertex_offset;

  float4 v0_pu = asfloat(vertex_buf.Load4(i0 * 48u + 0u));
  float4 v0_nv = asfloat(vertex_buf.Load4(i0 * 48u + 16u));
  float4 v0_co = asfloat(vertex_buf.Load4(i0 * 48u + 32u));

  float4 v1_pu = asfloat(vertex_buf.Load4(i1 * 48u + 0u));
  float4 v1_nv = asfloat(vertex_buf.Load4(i1 * 48u + 16u));
  float4 v1_co = asfloat(vertex_buf.Load4(i1 * 48u + 32u));

  float4 v2_pu = asfloat(vertex_buf.Load4(i2 * 48u + 0u));
  float4 v2_nv = asfloat(vertex_buf.Load4(i2 * 48u + 16u));
  float4 v2_co = asfloat(vertex_buf.Load4(i2 * 48u + 32u));

  float3 wp0 = mul_mat4_pt(instance_buf, inst_off, v0_pu.xyz);
  float3 wp1 = mul_mat4_pt(instance_buf, inst_off, v1_pu.xyz);
  float3 wp2 = mul_mat4_pt(instance_buf, inst_off, v2_pu.xyz);

  float4 c0 = mul(view_proj, float4(wp0, 1.0));
  float4 c1 = mul(view_proj, float4(wp1, 1.0));
  float4 c2 = mul(view_proj, float4(wp2, 1.0));

  float3 inv_w = float3(1.0 / max(abs(c0.w), 1e-6),
                        1.0 / max(abs(c1.w), 1e-6),
                        1.0 / max(abs(c2.w), 1e-6));
  float2 ndc0 = c0.xy * inv_w.x;
  float2 ndc1 = c1.xy * inv_w.y;
  float2 ndc2 = c2.xy * inv_w.z;

  float2 res = float2(viewport_grid.xy);
  float2 uv_screen = (float2(px) + 0.5) / res;
  float2 ndc_p = float2(uv_screen.x * 2.0 - 1.0, 1.0 - uv_screen.y * 2.0);

  float2 e10 = ndc1 - ndc0;
  float2 e20 = ndc2 - ndc0;
  float area = e10.x * e20.y - e10.y * e20.x;
  float inv_area = (abs(area) > 1e-10) ? (1.0 / area) : 0.0;

  float2 d = ndc_p - ndc0;
  float l1_s = (d.x * e20.y - d.y * e20.x) * inv_area;
  float l2_s = (e10.x * d.y - e10.y * d.x) * inv_area;
  float l0_s = 1.0 - l1_s - l2_s;

  float3 w_bary = float3(l0_s * inv_w.x, l1_s * inv_w.y, l2_s * inv_w.z);
  float w_sum = w_bary.x + w_bary.y + w_bary.z;
  float3 bary = (abs(w_sum) > 1e-10) ? (w_bary / w_sum) : float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);

  float2 d_ndc_dx = float2(2.0 / res.x, 0.0);
  float2 uv0 = float2(v0_pu.w, v0_nv.w);
  float2 uv1 = float2(v1_pu.w, v1_nv.w);
  float2 uv2 = float2(v2_pu.w, v2_nv.w);
  float2 uv_interp = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
  float dl1_dx = (d_ndc_dx.x * e20.y - d_ndc_dx.y * e20.x) * inv_area;
  float dl2_dx = (e10.x * d_ndc_dx.y - e10.y * d_ndc_dx.x) * inv_area;
  float2 duv_dx = (uv1 - uv0) * dl1_dx + (uv2 - uv0) * dl2_dx;
  float mip_hint = saturate(length(duv_dx) * 8.0);

  float3 local_pos = v0_pu.xyz * bary.x + v1_pu.xyz * bary.y + v2_pu.xyz * bary.z;
  float3 world_pos = wp0 * bary.x + wp1 * bary.y + wp2 * bary.z;
  float3 local_n = v0_nv.xyz * bary.x + v1_nv.xyz * bary.y + v2_nv.xyz * bary.z;
  float3 N = normalize(mul_mat3_vec(instance_buf, inst_off, local_n));
  float4 vcol = v0_co * bary.x + v1_co * bary.y + v2_co * bary.z;
  float3 V = normalize(camera_pos_znear.xyz - world_pos);

  float4 mat_base = asfloat(material_buf.Load4(mat_off + 0u));
  float4 mat_em_r = asfloat(material_buf.Load4(mat_off + 16u));
  float4 mat_mc = asfloat(material_buf.Load4(mat_off + 32u));

  float3 albedo = mat_base.rgb * vcol.rgb;
  float roughness = mat_em_r.w;
  float metallic = mat_mc.x;
  float clearcoat = mat_mc.y;

  if (active_material_class == 0u) {
    float grain = 0.92 + 0.08 * sin(uv_interp.x * 12.0) * cos(uv_interp.y * 12.0) * (1.0 - mip_hint);
    albedo *= grain;
    roughness = clamp(roughness + 0.12, 0.25, 0.98);
  } else if (active_material_class == 1u) {
    roughness = clamp(roughness, 0.18, 0.85);
  } else if (active_material_class == 2u) {
    metallic = 0.0;
    clearcoat = 0.0;
  } else if (active_material_class == 3u) {
    clearcoat = max(clearcoat, 0.85);
  }

  float3 sun_L = normalize(sun_dir_zfar.xyz);
  float3 lit = eval_ggx_brdf(N, V, sun_L, albedo, roughness, metallic, clearcoat) *
               sun_color_intensity.rgb * sun_color_intensity.w;
  float3 sky_amb = lerp(float3(0.06, 0.05, 0.04), float3(0.12, 0.16, 0.22), N.y * 0.5 + 0.5);
  lit += albedo * sky_amb * (1.0 - 0.5 * metallic) + mat_em_r.rgb;

  // Clustered Forward+ 3D Froxel lookup.
  float3 view_pos = mul(view, float4(world_pos, 1.0)).xyz;
  float view_depth = max(-view_pos.z, camera_pos_znear.w);
  float z_ratio = max(sun_dir_zfar.w / max(camera_pos_znear.w, 1e-4), 1.001);
  uint cz = uint(clamp(floor(log(view_depth / max(camera_pos_znear.w, 1e-4)) /
                             log(z_ratio) * float(grid_z_pad.x)),
                       0.0, float(grid_z_pad.x - 1u)));
  uint cx = min(uint(uv_screen.x * float(viewport_grid.z)), viewport_grid.z - 1u);
  uint cy = min(uint(uv_screen.y * float(viewport_grid.w)), viewport_grid.w - 1u);
  uint cluster_idx = cx + cy * viewport_grid.z + cz * (viewport_grid.z * viewport_grid.w);

  uint2 cell = cell_buf.Load2(cluster_idx * 8u);
  for (uint i = 0u; i < cell.y; ++i) {
    uint lid = light_idx_buf.Load((cell.x + i) * 4u);
    uint loff = lid * 64u;
    float4 l_pr = asfloat(light_buf.Load4(loff + 0u));
    float4 l_ci = asfloat(light_buf.Load4(loff + 16u));
    float4 l_do = asfloat(light_buf.Load4(loff + 32u));
    uint2 l_it = light_buf.Load2(loff + 48u);
    float cos_inner = asfloat(l_it.x);
    uint l_type = l_it.y;

    float3 to_l = l_pr.xyz - world_pos;
    float dist = length(to_l);
    float radius = max(l_pr.w, 1e-3);
    if (dist < radius && dist > 1e-4) {
      float3 L = to_l / dist;
      float atten = saturate(1.0 - dist / radius);
      atten = atten * atten / (dist * dist + 1.0);
      if (l_type == 1u) {
        float cos_ang = dot(-L, normalize(l_do.xyz));
        float spot = saturate((cos_ang - l_do.w) / max(cos_inner - l_do.w, 1e-4));
        atten *= spot * spot;
      }
      lit += eval_ggx_brdf(N, V, L, albedo, roughness, metallic, clearcoat) *
             l_ci.rgb * (l_ci.w * atten);
    }
  }

  float3 prev_wp = mul_mat4_pt(instance_buf, inst_off + 64u, local_pos);
  float4 prev_clip = mul(prev_view_proj, float4(prev_wp, 1.0));
  float2 prev_ndc = prev_clip.xy / max(abs(prev_clip.w), 1e-6);
  float2 prev_uv = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
  float2 velocity = uv_screen - prev_uv;

  out_hdr[px] = float4(lit, 1.0);
  out_gbuf[px] = float4(encode_octahedral(N), roughness, metallic);
  out_velocity[px] = velocity;
}
)";

inline constexpr std::string_view kVisShadeCompMsl = R"(
// shift_local_size(8, 8, 1)
#include <metal_stdlib>
using namespace metal;
struct ShadePush { uint active_material_class; uint max_tiles_per_class; };
struct ShadeParams { float4x4 view; float4x4 view_proj; float4x4 prev_view_proj; float4 camera_pos_znear; float4 sun_dir_zfar; float4 sun_color_intensity; uint4 viewport_grid; uint4 grid_z_pad; };
struct VisVertex { float4 pos_u; float4 norm_v; float4 color; };
struct Meshlet { float4 bs; float4 ca; packed_float3 axis; uint index_offset; uint vertex_offset; uint tc; uint vc; uint mid; };
struct MeshletInstance { float4x4 model; float4x4 prev_model; uint fm; uint mc; uint mcls; uint mid; };
struct VisibleDraw { uint instance_id; uint meshlet_id; uint material_class; uint material_id; };
struct VisMaterial { float4 base_color; float4 emissive_rough; float metallic; float clearcoat; uint mcls; uint slot; };
struct ClusterLight { float4 pos_radius; float4 color_intensity; float4 dir_cos_outer; float cos_inner; uint type; uint p0; uint p1; };
struct ClusterCell { uint offset; uint count; };

static float2 encode_octahedral(float3 n) {
  n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-6f);
  if (n.z < 0.0f) {
    float2 sgn = float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    n.xy = (1.0f - abs(n.yx)) * sgn;
  }
  return n.xy * 0.5f + 0.5f;
}

static float3 eval_ggx_brdf(float3 N, float3 V, float3 L, float3 albedo, float roughness, float metallic, float clearcoat) {
  float ndotl = max(dot(N, L), 0.0f);
  if (ndotl <= 0.0f) return float3(0.0f);
  float3 H = normalize(V + L);
  float ndotv = max(dot(N, V), 1e-4f);
  float ndoth = max(dot(N, H), 0.0f);
  float vdoth = max(dot(V, H), 0.0f);
  float a = max(roughness * roughness, 0.04f);
  float a2 = a * a;
  float denom = ndoth * ndoth * (a2 - 1.0f) + 1.0f;
  float D = a2 / (3.14159265f * denom * denom + 1e-6f);
  float k = roughness + 1.0f;
  k = (k * k) * 0.125f;
  float G = (ndotv / (ndotv * (1.0f - k) + k)) * (ndotl / (ndotl * (1.0f - k) + k));
  float3 F0 = mix(float3(0.04f), albedo, metallic);
  float3 F = F0 + (1.0f - F0) * pow(1.0f - vdoth, 5.0f);
  float3 spec = (D * G * F) / (4.0f * ndotv * ndotl + 1e-5f);
  float3 kd = (1.0f - F) * (1.0f - metallic);
  float3 diff = kd * albedo * (1.0f / 3.14159265f);
  float3 res = (diff + spec) * ndotl;
  if (clearcoat > 0.01f) {
    float ca = 0.06f;
    float ca2 = ca * ca;
    float cden = ndoth * ndoth * (ca2 - 1.0f) + 1.0f;
    float cD = ca2 / (3.14159265f * cden * cden + 1e-6f);
    float cF = 0.04f + 0.96f * pow(1.0f - vdoth, 5.0f);
    res += float3(cD * cF * 0.25f * clearcoat * ndotl);
  }
  return res;
}

kernel void main0(constant ShadePush& pc [[buffer(30)]],
                  constant ShadeParams& u [[buffer(0)]],
                  texture2d<uint, access::read> vis_buf [[texture(1)]],
                  device const uint* tile_lists [[buffer(2)]],
                  device const VisVertex* vertices [[buffer(3)]],
                  device const uint* indices [[buffer(4)]],
                  device const Meshlet* meshlets [[buffer(5)]],
                  device const MeshletInstance* instances [[buffer(6)]],
                  device const VisibleDraw* vis_draws [[buffer(7)]],
                  device const VisMaterial* materials [[buffer(8)]],
                  device const ClusterLight* lights [[buffer(9)]],
                  device const ClusterCell* cells [[buffer(10)]],
                  device const uint* light_indices [[buffer(11)]],
                  texture2d<float, access::write> out_hdr [[texture(12)]],
                  texture2d<float, access::write> out_gbuf [[texture(13)]],
                  texture2d<float, access::write> out_vel [[texture(14)]],
                  uint2 gid [[threadgroup_position_in_grid]],
                  uint2 ltid [[thread_position_in_threadgroup]]) {
  uint packed = tile_lists[pc.active_material_class * pc.max_tiles_per_class + gid.x];
  uint2 px = uint2(packed & 0xFFFFu, packed >> 16u) * 8u + ltid;
  if (px.x >= u.viewport_grid.x || px.y >= u.viewport_grid.y) return;
  uint2 vis = vis_buf.read(px).xy;
  if (vis.x == 0u || (vis.y >> 16u) != pc.active_material_class) return;
  VisibleDraw vd = vis_draws[(vis.x >> 8u) - 1u];
  MeshletInstance inst = instances[vd.instance_id];
  Meshlet m = meshlets[vd.meshlet_id];
  VisMaterial mat = materials[vd.material_id];
  uint ib = m.index_offset + (vis.x & 0xFFu) * 3u;
  VisVertex v0 = vertices[indices[ib] + m.vertex_offset];
  VisVertex v1 = vertices[indices[ib + 1u] + m.vertex_offset];
  VisVertex v2 = vertices[indices[ib + 2u] + m.vertex_offset];

  float3 wp0 = (inst.model * float4(v0.pos_u.xyz, 1.0f)).xyz;
  float3 wp1 = (inst.model * float4(v1.pos_u.xyz, 1.0f)).xyz;
  float3 wp2 = (inst.model * float4(v2.pos_u.xyz, 1.0f)).xyz;
  float4 c0 = u.view_proj * float4(wp0, 1.0f);
  float4 c1 = u.view_proj * float4(wp1, 1.0f);
  float4 c2 = u.view_proj * float4(wp2, 1.0f);
  float3 inv_w = float3(1.0f / max(abs(c0.w), 1e-6f),
                        1.0f / max(abs(c1.w), 1e-6f),
                        1.0f / max(abs(c2.w), 1e-6f));
  float2 ndc0 = c0.xy * inv_w.x;
  float2 ndc1 = c1.xy * inv_w.y;
  float2 ndc2 = c2.xy * inv_w.z;

  float2 res = float2(u.viewport_grid.xy);
  float2 uv_screen = (float2(px) + 0.5f) / res;
  float2 ndc_p = float2(uv_screen.x * 2.0f - 1.0f, 1.0f - uv_screen.y * 2.0f);
  float2 e10 = ndc1 - ndc0;
  float2 e20 = ndc2 - ndc0;
  float area = e10.x * e20.y - e10.y * e20.x;
  float inv_area = (abs(area) > 1e-10f) ? (1.0f / area) : 0.0f;
  float2 d = ndc_p - ndc0;
  float l1_s = (d.x * e20.y - d.y * e20.x) * inv_area;
  float l2_s = (e10.x * d.y - e10.y * d.x) * inv_area;
  float l0_s = 1.0f - l1_s - l2_s;
  float3 w_bary = float3(l0_s * inv_w.x, l1_s * inv_w.y, l2_s * inv_w.z);
  float w_sum = w_bary.x + w_bary.y + w_bary.z;
  float3 bary = (abs(w_sum) > 1e-10f) ? (w_bary / w_sum) : float3(1.0f / 3.0f);

  float2 d_ndc_dx = float2(2.0f / res.x, 0.0f);
  float2 uv0 = float2(v0.pos_u.w, v0.norm_v.w);
  float2 uv1 = float2(v1.pos_u.w, v1.norm_v.w);
  float2 uv2 = float2(v2.pos_u.w, v2.norm_v.w);
  float2 uv_interp = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
  float dl1_dx = (d_ndc_dx.x * e20.y - d_ndc_dx.y * e20.x) * inv_area;
  float dl2_dx = (e10.x * d_ndc_dx.y - e10.y * d_ndc_dx.x) * inv_area;
  float2 duv_dx = (uv1 - uv0) * dl1_dx + (uv2 - uv0) * dl2_dx;
  float mip_hint = clamp(length(duv_dx) * 8.0f, 0.0f, 1.0f);

  float3 local_pos = v0.pos_u.xyz * bary.x + v1.pos_u.xyz * bary.y + v2.pos_u.xyz * bary.z;
  float3 world_pos = wp0 * bary.x + wp1 * bary.y + wp2 * bary.z;
  float3 local_n = v0.norm_v.xyz * bary.x + v1.norm_v.xyz * bary.y + v2.norm_v.xyz * bary.z;
  float3 N = normalize(inst.model[0].xyz * local_n.x + inst.model[1].xyz * local_n.y + inst.model[2].xyz * local_n.z);
  float4 vcol = v0.color * bary.x + v1.color * bary.y + v2.color * bary.z;
  float3 V = normalize(u.camera_pos_znear.xyz - world_pos);

  float3 albedo = mat.base_color.rgb * vcol.rgb;
  float roughness = mat.emissive_rough.w;
  float metallic = mat.metallic;
  float clearcoat = mat.clearcoat;
  if (pc.active_material_class == 0u) {
    float grain = 0.92f + 0.08f * sin(uv_interp.x * 12.0f) * cos(uv_interp.y * 12.0f) * (1.0f - mip_hint);
    albedo *= grain;
    roughness = clamp(roughness + 0.12f, 0.25f, 0.98f);
  } else if (pc.active_material_class == 1u) {
    roughness = clamp(roughness, 0.18f, 0.85f);
  } else if (pc.active_material_class == 2u) {
    metallic = 0.0f;
    clearcoat = 0.0f;
  } else if (pc.active_material_class == 3u) {
    clearcoat = max(clearcoat, 0.85f);
  }

  float3 sun_L = normalize(u.sun_dir_zfar.xyz);
  float3 lit = eval_ggx_brdf(N, V, sun_L, albedo, roughness, metallic, clearcoat) *
               u.sun_color_intensity.rgb * u.sun_color_intensity.w;
  float3 sky_amb = mix(float3(0.06f, 0.05f, 0.04f), float3(0.12f, 0.16f, 0.22f), N.y * 0.5f + 0.5f);
  lit += albedo * sky_amb * (1.0f - 0.5f * metallic) + mat.emissive_rough.rgb;

  float3 view_pos = (u.view * float4(world_pos, 1.0f)).xyz;
  float view_depth = max(-view_pos.z, u.camera_pos_znear.w);
  float z_ratio = max(u.sun_dir_zfar.w / max(u.camera_pos_znear.w, 1e-4f), 1.001f);
  uint cz = uint(clamp(floor(log(view_depth / max(u.camera_pos_znear.w, 1e-4f)) / log(z_ratio) * float(u.grid_z_pad.x)),
                       0.0f, float(u.grid_z_pad.x - 1u)));
  uint cx = min(uint(uv_screen.x * float(u.viewport_grid.z)), u.viewport_grid.z - 1u);
  uint cy = min(uint(uv_screen.y * float(u.viewport_grid.w)), u.viewport_grid.w - 1u);
  ClusterCell cell = cells[cx + cy * u.viewport_grid.z + cz * (u.viewport_grid.z * u.viewport_grid.w)];
  for (uint i = 0u; i < cell.count; ++i) {
    ClusterLight l = lights[light_indices[cell.offset + i]];
    float3 to_l = l.pos_radius.xyz - world_pos;
    float dist = length(to_l);
    float radius = max(l.pos_radius.w, 1e-3f);
    if (dist < radius && dist > 1e-4f) {
      float3 L = to_l / dist;
      float atten = clamp(1.0f - dist / radius, 0.0f, 1.0f);
      atten = atten * atten / (dist * dist + 1.0f);
      if (l.type == 1u) {
        float cos_ang = dot(-L, normalize(l.dir_cos_outer.xyz));
        float spot = clamp((cos_ang - l.dir_cos_outer.w) / max(l.cos_inner - l.dir_cos_outer.w, 1e-4f), 0.0f, 1.0f);
        atten *= spot * spot;
      }
      lit += eval_ggx_brdf(N, V, L, albedo, roughness, metallic, clearcoat) * l.color_intensity.rgb * (l.color_intensity.w * atten);
    }
  }

  float3 prev_wp = (inst.prev_model * float4(local_pos, 1.0f)).xyz;
  float4 prev_clip = u.prev_view_proj * float4(prev_wp, 1.0f);
  float2 prev_ndc = prev_clip.xy / max(abs(prev_clip.w), 1e-6f);
  float2 prev_uv = float2(prev_ndc.x * 0.5f + 0.5f, 0.5f - prev_ndc.y * 0.5f);
  float2 velocity = uv_screen - prev_uv;

  out_hdr.write(float4(lit, 1.0f), px);
  out_gbuf.write(float4(encode_octahedral(N), roughness, metallic), px);
  out_vel.write(float4(velocity, 0.0f, 0.0f), px);
}
)";

// =============================================================================
// 6. GPU Compute Xenos 2D Detiler + Endian Swap + Cutout Check (xenos_detile.comp)
// =============================================================================
inline constexpr std::string_view kXenosDetileCompHlsl = R"(
cbuffer DetileParams : register(b0, space15) {
  uint width_blocks;
  uint height_blocks;
  uint pitch_blocks;
  uint block_offset_x;
  uint block_offset_y;
  uint block_bytes;
  uint log2_block_bytes;
  uint endian_mask;
  uint src_byte_offset;
  uint dst_byte_offset;
  uint tiled;
  uint format_kind;
};

ByteAddressBuffer src_buf : register(t0, space0);
RWByteAddressBuffer dst_buf : register(u1, space0);
RWByteAddressBuffer cutout_buf : register(u2, space0);

uint xenos_tiled_offset_2d(uint x, uint y, uint pitch, uint log2_bb) {
  uint p = (pitch + 31u) & ~31u;
  uint macro_o = ((x >> 5u) + (y >> 5u) * (p >> 5u)) << (log2_bb + 7u);
  uint micro_o = ((x & 7u) + ((y & 0xEu) << 2u)) << log2_bb;
  uint offset = macro_o + ((micro_o & ~0xFu) << 1u) + (micro_o & 0xFu) + ((y & 1u) << 4u);
  return ((offset & ~0x1FFu) << 3u) + ((y & 16u) << 7u) + ((offset & 0x1C0u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (offset & 0x3Fu);
}

uint swap_word_by_endian_mask(uint w, uint mask) {
  if (mask == 0u) return w;
  if (mask == 1u) return ((w & 0x00FF00FFu) << 8u) | ((w & 0xFF00FF00u) >> 8u);
  if (mask == 3u) {
    return ((w & 0x000000FFu) << 24u) | ((w & 0x0000FF00u) << 8u) |
           ((w & 0x00FF0000u) >> 8u)  | ((w & 0xFF000000u) >> 24u);
  }
  return (w << 16u) | (w >> 16u);
}

[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_DispatchThreadID) {
  uint bx = gtid.x;
  uint by = gtid.y;
  if (bx >= width_blocks || by >= height_blocks) return;

  uint sx = bx + block_offset_x;
  uint sy = by + block_offset_y;
  uint src_off = src_byte_offset +
      ((tiled != 0u)
          ? xenos_tiled_offset_2d(sx, sy, pitch_blocks, log2_block_bytes)
          : (sy * pitch_blocks + sx) * block_bytes);
  uint dst_off = dst_byte_offset + (by * width_blocks + bx) * block_bytes;

  uint words_per_block = block_bytes >> 2u;
  uint w0 = swap_word_by_endian_mask(src_buf.Load(src_off + 0u), endian_mask);
  dst_buf.Store(dst_off + 0u, w0);
  uint w1 = 0u;
  if (words_per_block >= 2u) {
    w1 = swap_word_by_endian_mask(src_buf.Load(src_off + 4u), endian_mask);
    dst_buf.Store(dst_off + 4u, w1);
  }
  if (words_per_block >= 4u) {
    dst_buf.Store(dst_off + 8u, swap_word_by_endian_mask(src_buf.Load(src_off + 8u), endian_mask));
    dst_buf.Store(dst_off + 12u, swap_word_by_endian_mask(src_buf.Load(src_off + 12u), endian_mask));
  }

  bool has_cutout = false;
  if (format_kind == 0u) {
    has_cutout = ((w0 >> 24u) < 128u);
  } else if (format_kind == 1u) {
    uint c0 = w0 & 0xFFFFu;
    uint c1 = w0 >> 16u;
    if (c0 <= c1) {
      uint both_bits = (w1 & 0x55555555u) & ((w1 >> 1u) & 0x55555555u);
      has_cutout = (both_bits != 0u);
    }
  } else if (format_kind == 2u) {
    for (uint i = 0u; i < 8u; ++i) {
      if (((w0 >> (i * 4u)) & 0xFu) < 8u || ((w1 >> (i * 4u)) & 0xFu) < 8u) {
        has_cutout = true;
        break;
      }
    }
  } else if (format_kind == 3u) {
    uint a0 = w0 & 0xFFu;
    uint a1 = (w0 >> 8u) & 0xFFu;
    if (a0 < 128u || a1 < 128u) {
      has_cutout = true;
    } else if (a0 <= a1) {
      uint lo = (w0 >> 16u) | (w1 << 16u);
      uint hi = w1 >> 16u;
      for (uint t = 0u; t < 16u; ++t) {
        uint bit_off = t * 3u;
        uint idx = (bit_off < 30u) ? ((lo >> bit_off) & 7u)
                 : (bit_off == 30u) ? (((lo >> 30u) & 3u) | ((hi & 1u) << 2u))
                                    : ((hi >> (bit_off - 32u)) & 7u);
        if (idx == 6u) {
          has_cutout = true;
          break;
        }
      }
    }
  }
  if (has_cutout) {
    uint prev;
    cutout_buf.InterlockedOr(0u, 1u, prev);
  }
}
)";

inline constexpr std::string_view kXenosDetileCompMsl = R"(
// shift_local_size(8, 8, 1)
#include <metal_stdlib>
using namespace metal;
struct DetileParams { uint wb; uint hb; uint pitch; uint bx0; uint by0; uint bb; uint l2; uint mask; uint soff; uint doff; uint tiled; uint kind; };
static uint xenos_off(uint x, uint y, uint pitch, uint l2) {
  uint p = (pitch + 31u) & ~31u;
  uint ma = ((x >> 5u) + (y >> 5u) * (p >> 5u)) << (l2 + 7u);
  uint mi = ((x & 7u) + ((y & 0xEu) << 2u)) << l2;
  uint o = ma + ((mi & ~0xFu) << 1u) + (mi & 0xFu) + ((y & 1u) << 4u);
  return ((o & ~0x1FFu) << 3u) + ((y & 16u) << 7u) + ((o & 0x1C0u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (o & 0x3Fu);
}
static uint swp(uint w, uint m) {
  if (m == 0u) return w;
  if (m == 1u) return ((w & 0x00FF00FFu) << 8u) | ((w & 0xFF00FF00u) >> 8u);
  if (m == 3u) return ((w & 0xFFu) << 24u) | ((w & 0xFF00u) << 8u) | ((w & 0xFF0000u) >> 8u) | (w >> 24u);
  return (w << 16u) | (w >> 16u);
}
kernel void main0(constant DetileParams& pc [[buffer(30)]],
                  device const uint* src [[buffer(0)]],
                  device uint* dst [[buffer(1)]],
                  device atomic_uint* cutout [[buffer(2)]],
                  uint2 tid [[thread_position_in_grid]]) {
  if (tid.x >= pc.wb || tid.y >= pc.hb) return;
  uint sx = tid.x + pc.bx0, sy = tid.y + pc.by0;
  uint so = (pc.soff + (pc.tiled ? xenos_off(sx, sy, pc.pitch, pc.l2) : (sy * pc.pitch + sx) * pc.bb)) >> 2u;
  uint doff = (pc.doff + (tid.y * pc.wb + tid.x) * pc.bb) >> 2u;
  uint n = pc.bb >> 2u;
  uint w0 = swp(src[so], pc.mask); dst[doff] = w0;
  uint w1 = 0; if (n >= 2u) { w1 = swp(src[so + 1u], pc.mask); dst[doff + 1u] = w1; }
  if (n >= 4u) { dst[doff + 2u] = swp(src[so + 2u], pc.mask); dst[doff + 3u] = swp(src[so + 3u], pc.mask); }
  bool has_cutout = false;
  if (pc.kind == 0u) {
    has_cutout = ((w0 >> 24u) < 128u);
  } else if (pc.kind == 1u) {
    uint c0 = w0 & 0xFFFFu;
    uint c1 = w0 >> 16u;
    if (c0 <= c1) {
      uint both_bits = (w1 & 0x55555555u) & ((w1 >> 1u) & 0x55555555u);
      has_cutout = (both_bits != 0u);
    }
  } else if (pc.kind == 2u) {
    for (uint i = 0u; i < 8u; ++i) {
      if (((w0 >> (i * 4u)) & 0xFu) < 8u || ((w1 >> (i * 4u)) & 0xFu) < 8u) {
        has_cutout = true;
        break;
      }
    }
  } else if (pc.kind == 3u) {
    uint a0 = w0 & 0xFFu;
    uint a1 = (w0 >> 8u) & 0xFFu;
    if (a0 < 128u || a1 < 128u) {
      has_cutout = true;
    } else if (a0 <= a1) {
      uint lo = (w0 >> 16u) | (w1 << 16u);
      uint hi = w1 >> 16u;
      for (uint t = 0u; t < 16u; ++t) {
        uint bit_off = t * 3u;
        uint idx = (bit_off < 30u) ? ((lo >> bit_off) & 7u)
                 : (bit_off == 30u) ? (((lo >> 30u) & 3u) | ((hi & 1u) << 2u))
                                    : ((hi >> (bit_off - 32u)) & 7u);
        if (idx == 6u) {
          has_cutout = true;
          break;
        }
      }
    }
  }
  if (has_cutout) {
    atomic_fetch_or_explicit(cutout, 1u, memory_order_relaxed);
  }
}
)";

// =============================================================================
// 7. Single-Pass LDS Hi-Z Pyramid Downsampler (hiz_spd.comp)
// =============================================================================
inline constexpr std::string_view kHizSpdCompHlsl = R"(
cbuffer HizSpdParams : register(b0, space15) {
  uint src_width;
  uint src_height;
  uint num_mips;
  uint is_reverse_z;
};
Texture2D<float> src_depth : register(t0, space0);
RWTexture2D<float> dst_mip0 : register(u1, space0);
RWTexture2D<float> dst_mip1 : register(u2, space0);
RWTexture2D<float> dst_mip2 : register(u3, space0);
RWTexture2D<float> dst_mip3 : register(u4, space0);

groupshared float s_tile[8][8];

float reduce2(float a, float b) { return (is_reverse_z != 0u) ? min(a, b) : max(a, b); }
float reduce4(float a, float b, float c, float d) { return reduce2(reduce2(a, b), reduce2(c, d)); }
float load_src(int2 p) {
  int2 c = clamp(p, int2(0, 0), int2(int(src_width) - 1, int(src_height) - 1));
  return src_depth.Load(int3(c, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 lid3 : SV_GroupThreadID, uint3 gid3 : SV_GroupID) {
  uint2 lid = lid3.xy;
  uint2 gid = gid3.xy;
  int2 d0 = max(int2(src_width >> 1u, src_height >> 1u), int2(1, 1));
  int2 p0 = int2(gid * 8u + lid);
  int2 base = p0 * 2;
  float m0 = reduce4(load_src(base), load_src(base + int2(1, 0)),
                     load_src(base + int2(0, 1)), load_src(base + int2(1, 1)));
  bool extra_x = ((src_width & 1u) != 0u) && (p0.x == d0.x - 1) && (src_width > 1u);
  bool extra_y = ((src_height & 1u) != 0u) && (p0.y == d0.y - 1) && (src_height > 1u);
  if (extra_x) {
    m0 = reduce2(m0, reduce2(load_src(base + int2(2, 0)), load_src(base + int2(2, 1))));
  }
  if (extra_y) {
    m0 = reduce2(m0, reduce2(load_src(base + int2(0, 2)), load_src(base + int2(1, 2))));
  }
  if (extra_x && extra_y) {
    m0 = reduce2(m0, load_src(base + int2(2, 2)));
  }
  if (num_mips >= 1u && p0.x < d0.x && p0.y < d0.y) dst_mip0[p0] = m0;
  s_tile[lid.y][lid.x] = m0;
  GroupMemoryBarrierWithGroupSync();

  if (num_mips >= 2u && lid.x < 4u && lid.y < 4u) {
    uint2 q = lid * 2u;
    float m1 = reduce4(s_tile[q.y][q.x], s_tile[q.y][q.x + 1u],
                       s_tile[q.y + 1u][q.x], s_tile[q.y + 1u][q.x + 1u]);
    int2 d1 = max(d0 >> 1, int2(1, 1));
    int2 p1 = int2(gid * 4u + lid);
    if (p1.x < d1.x && p1.y < d1.y) dst_mip1[p1] = m1;
    s_tile[lid.y][lid.x] = m1;
  }
  GroupMemoryBarrierWithGroupSync();

  if (num_mips >= 3u && lid.x < 2u && lid.y < 2u) {
    uint2 q = lid * 2u;
    float m2 = reduce4(s_tile[q.y][q.x], s_tile[q.y][q.x + 1u],
                       s_tile[q.y + 1u][q.x], s_tile[q.y + 1u][q.x + 1u]);
    int2 d2 = max(d0 >> 2, int2(1, 1));
    int2 p2 = int2(gid * 2u + lid);
    if (p2.x < d2.x && p2.y < d2.y) dst_mip2[p2] = m2;
    s_tile[lid.y][lid.x] = m2;
  }
  GroupMemoryBarrierWithGroupSync();

  if (num_mips >= 4u && lid.x == 0u && lid.y == 0u) {
    float m3 = reduce4(s_tile[0][0], s_tile[0][1], s_tile[1][0], s_tile[1][1]);
    int2 d3 = max(d0 >> 3, int2(1, 1));
    int2 p3 = int2(gid);
    if (p3.x < d3.x && p3.y < d3.y) dst_mip3[p3] = m3;
  }
}
)";

inline constexpr std::string_view kHizSpdCompMsl = R"(
// shift_local_size(8, 8, 1)
#include <metal_stdlib>
using namespace metal;
struct HizSpdParams { uint sw; uint sh; uint nm; uint revz; };
static float r2(float a, float b, uint revz) { return revz ? min(a, b) : max(a, b); }
kernel void main0(constant HizSpdParams& pc [[buffer(30)]],
                  texture2d<float, access::read> src [[texture(0)]],
                  texture2d<float, access::write> m0t [[texture(1)]],
                  texture2d<float, access::write> m1t [[texture(2)]],
                  texture2d<float, access::write> m2t [[texture(3)]],
                  texture2d<float, access::write> m3t [[texture(4)]],
                  uint2 lid [[thread_position_in_threadgroup]],
                  uint2 gid [[threadgroup_position_in_grid]]) {
  threadgroup float s[8][8];
  uint2 mx = uint2(pc.sw - 1u, pc.sh - 1u);
  uint2 d0 = max(uint2(pc.sw >> 1u, pc.sh >> 1u), uint2(1u));
  uint2 p0 = gid * 8u + lid;
  uint2 b = p0 * 2u;
  float v = r2(r2(src.read(min(b, mx)).x, src.read(min(b + uint2(1, 0), mx)).x, pc.revz),
               r2(src.read(min(b + uint2(0, 1), mx)).x, src.read(min(b + uint2(1, 1), mx)).x, pc.revz), pc.revz);
  bool extra_x = ((pc.sw & 1u) != 0u) && (p0.x == d0.x - 1u) && (pc.sw > 1u);
  bool extra_y = ((pc.sh & 1u) != 0u) && (p0.y == d0.y - 1u) && (pc.sh > 1u);
  if (extra_x) {
    v = r2(v, r2(src.read(min(b + uint2(2, 0), mx)).x, src.read(min(b + uint2(2, 1), mx)).x, pc.revz), pc.revz);
  }
  if (extra_y) {
    v = r2(v, r2(src.read(min(b + uint2(0, 2), mx)).x, src.read(min(b + uint2(1, 2), mx)).x, pc.revz), pc.revz);
  }
  if (extra_x && extra_y) {
    v = r2(v, src.read(min(b + uint2(2, 2), mx)).x, pc.revz);
  }
  if (pc.nm >= 1u && p0.x < d0.x && p0.y < d0.y) m0t.write(float4(v, 0, 0, 0), p0);
  s[lid.y][lid.x] = v; threadgroup_barrier(mem_flags::mem_threadgroup);
  if (pc.nm >= 2u && lid.x < 4u && lid.y < 4u) {
    uint2 q = lid * 2u;
    float v1 = r2(r2(s[q.y][q.x], s[q.y][q.x+1], pc.revz), r2(s[q.y+1][q.x], s[q.y+1][q.x+1], pc.revz), pc.revz);
    uint2 p1 = gid * 4u + lid, d1 = max(d0 >> 1u, uint2(1u));
    if (p1.x < d1.x && p1.y < d1.y) m1t.write(float4(v1, 0, 0, 0), p1);
    s[lid.y][lid.x] = v1;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (pc.nm >= 3u && lid.x < 2u && lid.y < 2u) {
    uint2 q = lid * 2u;
    float v2 = r2(r2(s[q.y][q.x], s[q.y][q.x+1], pc.revz), r2(s[q.y+1][q.x], s[q.y+1][q.x+1], pc.revz), pc.revz);
    uint2 p2 = gid * 2u + lid, d2 = max(d0 >> 2u, uint2(1u));
    if (p2.x < d2.x && p2.y < d2.y) m2t.write(float4(v2, 0, 0, 0), p2);
    s[lid.y][lid.x] = v2;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (pc.nm >= 4u && lid.x == 0u && lid.y == 0u) {
    float v3 = r2(r2(s[0][0], s[0][1], pc.revz), r2(s[1][0], s[1][1], pc.revz), pc.revz);
    uint2 d3 = max(d0 >> 3u, uint2(1u));
    if (gid.x < d3.x && gid.y < d3.y) m3t.write(float4(v3, 0, 0, 0), gid);
  }
}
)";

// =============================================================================
// 8. 2-Phase Hi-Z Occlusion Meshlet Cull (vis_cull_hiz.comp)
// =============================================================================
inline constexpr std::string_view kVisCullHizCompHlsl = R"(
cbuffer HizCullParams : register(b0, space0) {
  column_major float4x4 view_proj;
  float4 frustum_planes[6];
  float4 camera_pos_znear;
  uint work_item_count;
  uint max_draws;
  uint enable_cone_cull;
  uint enable_hiz_cull;
  uint phase;
  uint hiz_width;
  uint hiz_height;
  uint hiz_mip_count;
};
ByteAddressBuffer instances_buf : register(t1, space0);
ByteAddressBuffer meshlets_buf : register(t2, space0);
ByteAddressBuffer work_items_buf : register(t3, space0);
RWByteAddressBuffer draw_cmds_buf : register(u4, space0);
RWByteAddressBuffer vis_draws_buf : register(u5, space0);
RWByteAddressBuffer occluded_buf : register(u6, space0);
RWByteAddressBuffer stats_buf : register(u7, space0);
Texture2D<float> hiz_tex : register(t8, space0);

float4x4 load_mat4(ByteAddressBuffer b, uint off) {
  float4 c0 = asfloat(b.Load4(off + 0u));
  float4 c1 = asfloat(b.Load4(off + 16u));
  float4 c2 = asfloat(b.Load4(off + 32u));
  float4 c3 = asfloat(b.Load4(off + 48u));
  return float4x4(c0.x, c1.x, c2.x, c3.x,
                  c0.y, c1.y, c2.y, c3.y,
                  c0.z, c1.z, c2.z, c3.z,
                  c0.w, c1.w, c2.w, c3.w);
}

bool test_hiz_occluded(float3 world_center, float world_radius) {
  if (enable_hiz_cull == 0u || hiz_width == 0u || hiz_height == 0u) return false;
  float2 ndc_min = float2(1e9, 1e9);
  float2 ndc_max = float2(-1e9, -1e9);
  float nearest_z = 0.0;
  for (uint c = 0u; c < 8u; ++c) {
    float3 off = float3(((c & 1u) != 0u) ? world_radius : -world_radius,
                        ((c & 2u) != 0u) ? world_radius : -world_radius,
                        ((c & 4u) != 0u) ? world_radius : -world_radius);
    float4 clip = mul(view_proj, float4(world_center + off, 1.0));
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    ndc_min = min(ndc_min, ndc.xy);
    ndc_max = max(ndc_max, ndc.xy);
    nearest_z = max(nearest_z, ndc.z);
  }
  float2 uv_min = clamp(ndc_min * 0.5 + 0.5, 0.0, 1.0);
  float2 uv_max = clamp(ndc_max * 0.5 + 0.5, 0.0, 1.0);
  float v0 = 1.0 - uv_max.y;
  float v1 = 1.0 - uv_min.y;
  uv_min.y = v0;
  uv_max.y = v1;

  float2 px_span = (uv_max - uv_min) * float2(float(hiz_width), float(hiz_height));
  float max_span = max(px_span.x, px_span.y);
  int mip = clamp(int(ceil(log2(max(max_span, 1.0)))), 0, int(max(hiz_mip_count, 1u) - 1u));
  int2 mip_sz = max(int2(hiz_width >> mip, hiz_height >> mip), int2(1, 1));
  int2 tmin = clamp(int2(floor(uv_min * float2(mip_sz))), int2(0, 0), mip_sz - int2(1, 1));
  int2 tmax = clamp(int2(floor(uv_max * float2(mip_sz))), int2(0, 0), mip_sz - int2(1, 1));

  float d00 = hiz_tex.Load(int3(tmin.x, tmin.y, mip));
  float d10 = hiz_tex.Load(int3(tmax.x, tmin.y, mip));
  float d01 = hiz_tex.Load(int3(tmin.x, tmax.y, mip));
  float d11 = hiz_tex.Load(int3(tmax.x, tmax.y, mip));
  float hiz_min = min(min(d00, d10), min(d01, d11));
  return (nearest_z + 1e-4) < hiz_min;
}

void emit_draw(uint2 item, uint4 inst_meta, uint index_offset, uint vertex_offset, uint tri_count, uint slot) {
  if (slot >= max_draws) return;
  uint dc = slot * 20u;
  draw_cmds_buf.Store(dc + 0u, tri_count * 3u);
  draw_cmds_buf.Store(dc + 4u, 1u);
  draw_cmds_buf.Store(dc + 8u, index_offset);
  draw_cmds_buf.Store(dc + 12u, vertex_offset);
  draw_cmds_buf.Store(dc + 16u, slot);
  vis_draws_buf.Store4(slot * 16u, uint4(item.x, item.y, inst_meta.z, inst_meta.w));
}

[numthreads(64, 1, 1)]
void main(uint3 gtid : SV_DispatchThreadID) {
  uint tid = gtid.x;
  uint count = (phase == 0u) ? work_item_count : min(stats_buf.Load(16u), max_draws);
  if (tid >= count) return;

  uint2 item = (phase == 0u) ? work_items_buf.Load2(tid * 8u) : occluded_buf.Load2(tid * 8u);
  uint inst_off = item.x * 144u;
  float4x4 model = load_mat4(instances_buf, inst_off);
  uint4 inst_meta = instances_buf.Load4(inst_off + 128u);

  uint m_off = item.y * 64u;
  float4 bs = asfloat(meshlets_buf.Load4(m_off + 0u));
  float4 ca = asfloat(meshlets_buf.Load4(m_off + 16u));
  uint4 w2 = meshlets_buf.Load4(m_off + 32u);
  uint4 w3 = meshlets_buf.Load4(m_off + 48u);
  float3 cone_axis = asfloat(w2.xyz);

  float3 world_center = mul(model, float4(bs.xyz, 1.0)).xyz;
  float3 c0 = float3(model[0][0], model[1][0], model[2][0]);
  float3 c1 = float3(model[0][1], model[1][1], model[2][1]);
  float3 c2 = float3(model[0][2], model[1][2], model[2][2]);
  float world_radius = bs.w * max(length(c0), max(length(c1), length(c2)));

  uint prev;
  if (phase == 0u) {
    stats_buf.InterlockedAdd(12u, 1u, prev);
    for (uint p = 0u; p < 6u; ++p) {
      float4 pl = frustum_planes[p];
      if (dot(pl.xyz, world_center) + pl.w < -world_radius) {
        stats_buf.InterlockedAdd(4u, 1u, prev);
        return;
      }
    }
    if (enable_cone_cull != 0u && ca.w < 0.999) {
      float3 world_axis = normalize(mul((float3x3)model, cone_axis));
      float3 view_vec = world_center - camera_pos_znear.xyz;
      float dist = length(view_vec);
      if (dist > world_radius && dot(view_vec, world_axis) >= ca.w * dist + world_radius) {
        stats_buf.InterlockedAdd(8u, 1u, prev);
        return;
      }
    }
    if (test_hiz_occluded(world_center, world_radius)) {
      uint oslot;
      stats_buf.InterlockedAdd(16u, 1u, oslot);
      if (oslot < max_draws) occluded_buf.Store2(oslot * 8u, item);
      return;
    }
    uint slot;
    stats_buf.InterlockedAdd(0u, 1u, slot);
    stats_buf.InterlockedAdd(24u, 1u, prev);
    emit_draw(item, inst_meta, w2.w, w3.x, w3.y, slot);
  } else {
    if (test_hiz_occluded(world_center, world_radius)) {
      stats_buf.InterlockedAdd(28u, 1u, prev);
      return;
    }
    uint global_slot;
    stats_buf.InterlockedAdd(20u, 1u, prev);
    stats_buf.InterlockedAdd(24u, 1u, global_slot);
    emit_draw(item, inst_meta, w2.w, w3.x, w3.y, global_slot);
  }
}
)";

inline constexpr std::string_view kVisCullHizCompMsl = R"(
// shift_local_size(64, 1, 1)
#include <metal_stdlib>
using namespace metal;
struct HizCullParams { float4x4 vp; float4 pl[6]; float4 cam; uint n; uint maxd; uint cc; uint hc; uint ph; uint hw; uint hh; uint hm; };
struct MI { float4x4 m; float4x4 pm; uint fm; uint mc; uint mcls; uint mid; };
struct M { float4 bs; float4 ca; packed_float3 ax; uint io; uint vo; uint tc; uint vc; uint mid; };
struct D { uint ic; uint inst; uint fi; int vo; uint finst; };
struct VD { uint iid; uint mid; uint mcls; uint mat; };

static bool test_hiz_occluded(constant HizCullParams& u, texture2d<float, access::read> hiz,
                              float3 world_center, float world_radius) {
  if (u.hc == 0u || u.hw == 0u || u.hh == 0u) return false;
  float2 ndc_min = float2(1e9f);
  float2 ndc_max = float2(-1e9f);
  float nearest_z = 0.0f;
  for (uint c = 0u; c < 8u; ++c) {
    float3 off = float3((c & 1u) ? world_radius : -world_radius,
                        (c & 2u) ? world_radius : -world_radius,
                        (c & 4u) ? world_radius : -world_radius);
    float4 clip = u.vp * float4(world_center + off, 1.0f);
    if (clip.w <= 1e-4f) return false;
    float3 ndc = clip.xyz / clip.w;
    ndc_min = min(ndc_min, ndc.xy);
    ndc_max = max(ndc_max, ndc.xy);
    nearest_z = max(nearest_z, ndc.z);
  }
  float2 uv_min = clamp(ndc_min * 0.5f + 0.5f, 0.0f, 1.0f);
  float2 uv_max = clamp(ndc_max * 0.5f + 0.5f, 0.0f, 1.0f);
  float v0 = 1.0f - uv_max.y;
  float v1 = 1.0f - uv_min.y;
  uv_min.y = v0;
  uv_max.y = v1;

  float2 px_span = (uv_max - uv_min) * float2(float(u.hw), float(u.hh));
  float max_span = max(px_span.x, px_span.y);
  uint mip = uint(clamp(int(ceil(log2(max(max_span, 1.0f)))), 0, int(max(u.hm, 1u) - 1u)));
  uint2 mip_sz = max(uint2(u.hw >> mip, u.hh >> mip), uint2(1u));
  uint2 tmin = clamp(uint2(floor(uv_min * float2(mip_sz))), uint2(0u), mip_sz - uint2(1u));
  uint2 tmax = clamp(uint2(floor(uv_max * float2(mip_sz))), uint2(0u), mip_sz - uint2(1u));

  float d00 = hiz.read(uint2(tmin.x, tmin.y), mip).x;
  float d10 = hiz.read(uint2(tmax.x, tmin.y), mip).x;
  float d01 = hiz.read(uint2(tmin.x, tmax.y), mip).x;
  float d11 = hiz.read(uint2(tmax.x, tmax.y), mip).x;
  float hiz_min = min(min(d00, d10), min(d01, d11));
  return (nearest_z + 1e-4f) < hiz_min;
}

kernel void main0(constant HizCullParams& u [[buffer(0)]], device const MI* insts [[buffer(1)]],
                  device const M* ms [[buffer(2)]], device const uint2* items [[buffer(3)]],
                  device D* cmds [[buffer(4)]], device VD* vds [[buffer(5)]], device uint2* occ [[buffer(6)]],
                  device atomic_uint* st [[buffer(7)]], texture2d<float, access::read> hiz [[texture(8)]],
                  uint tid [[thread_position_in_grid]]) {
  uint cnt = (u.ph == 0u) ? u.n : min(atomic_load_explicit(&st[4], memory_order_relaxed), u.maxd);
  if (tid >= cnt) return;
  uint2 it = (u.ph == 0u) ? items[tid] : occ[tid];
  MI inst = insts[it.x]; M m = ms[it.y];
  float3 wc = (inst.m * float4(m.bs.xyz, 1.0f)).xyz;
  float wr = m.bs.w * max(length(inst.m[0].xyz), max(length(inst.m[1].xyz), length(inst.m[2].xyz)));
  if (u.ph == 0u) {
    atomic_fetch_add_explicit(&st[3], 1u, memory_order_relaxed);
    for (uint p = 0u; p < 6u; ++p) {
      if (dot(u.pl[p].xyz, wc) + u.pl[p].w < -wr) {
        atomic_fetch_add_explicit(&st[1], 1u, memory_order_relaxed);
        return;
      }
    }
    if (u.cc != 0u && m.ca.w < 0.999f) {
      float3 ax = float3(m.ax[0], m.ax[1], m.ax[2]);
      float3 wa = normalize(inst.m[0].xyz * ax.x + inst.m[1].xyz * ax.y + inst.m[2].xyz * ax.z);
      float3 vv = wc - u.cam.xyz;
      float dist = length(vv);
      if (dist > wr && dot(vv, wa) >= m.ca.w * dist + wr) {
        atomic_fetch_add_explicit(&st[2], 1u, memory_order_relaxed);
        return;
      }
    }
    if (test_hiz_occluded(u, hiz, wc, wr)) {
      uint oslot = atomic_fetch_add_explicit(&st[4], 1u, memory_order_relaxed);
      if (oslot < u.maxd) occ[oslot] = it;
      return;
    }
    uint slot = atomic_fetch_add_explicit(&st[0], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&st[6], 1u, memory_order_relaxed);
    if (slot < u.maxd) {
      cmds[slot] = {m.tc * 3u, 1u, m.io, int(m.vo), slot};
      vds[slot] = {it.x, it.y, inst.mcls, inst.mid};
    }
  } else {
    if (test_hiz_occluded(u, hiz, wc, wr)) {
      atomic_fetch_add_explicit(&st[7], 1u, memory_order_relaxed);
      return;
    }
    atomic_fetch_add_explicit(&st[5], 1u, memory_order_relaxed);
    uint global_slot = atomic_fetch_add_explicit(&st[6], 1u, memory_order_relaxed);
    if (global_slot < u.maxd) {
      cmds[global_slot] = {m.tc * 3u, 1u, m.io, int(m.vo), global_slot};
      vds[global_slot] = {it.x, it.y, inst.mcls, inst.mid};
    }
  }
}
)";

// =============================================================================
// 9. Async Compute Karis Bloom Downsample / Tent Upsample (post_bloom_spd.comp)
// =============================================================================
inline constexpr std::string_view kPostBloomSpdCompHlsl = R"(
cbuffer BloomParams : register(b0, space15) {
  uint dst_width;
  uint dst_height;
  uint src_width;
  uint src_height;
  uint mode;
  float threshold;
  float intensity;
  float pad;
};
Texture2D<float4> src_tex : register(t0, space0);
RWTexture2D<float4> dst_img : register(u1, space0);

float3 fetch_clamped(int2 p) {
  int2 c = clamp(p, int2(0, 0), int2(int(src_width) - 1, int(src_height) - 1));
  return src_tex.Load(int3(c, 0)).rgb;
}
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 karis_avg(float3 a, float3 b, float3 c, float3 d) {
  float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b));
  float wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));
  return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-4);
}

[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_DispatchThreadID) {
  int2 px = int2(gtid.xy);
  if (px.x >= int(dst_width) || px.y >= int(dst_height)) return;
  if (mode == 0u) {
    int2 s = px * 2;
    float3 avg = karis_avg(fetch_clamped(s), fetch_clamped(s + int2(1, 0)),
                           fetch_clamped(s + int2(0, 1)), fetch_clamped(s + int2(1, 1)));
    if (threshold > 0.0) {
      float lum = luma(avg);
      avg *= max(lum - threshold, 0.0) / max(lum, 1e-4);
    }
    dst_img[px] = float4(avg, 1.0);
  } else {
    int2 s = px / 2;
    float3 sum =
        fetch_clamped(s + int2(-1, -1)) * 1.0 + fetch_clamped(s + int2(0, -1)) * 2.0 + fetch_clamped(s + int2(1, -1)) * 1.0 +
        fetch_clamped(s + int2(-1,  0)) * 2.0 + fetch_clamped(s + int2(0,  0)) * 4.0 + fetch_clamped(s + int2(1,  0)) * 2.0 +
        fetch_clamped(s + int2(-1,  1)) * 1.0 + fetch_clamped(s + int2(0,  1)) * 2.0 + fetch_clamped(s + int2(1,  1)) * 1.0;
    float3 base = dst_img[px].rgb;
    dst_img[px] = float4(base + sum * (intensity / 16.0), 1.0);
  }
}
)";

inline constexpr std::string_view kPostBloomSpdCompMsl = R"(
// shift_local_size(8, 8, 1)
#include <metal_stdlib>
using namespace metal;
struct BP { uint dw; uint dh; uint sw; uint sh; uint mode; float th; float inten; float p; };
static float luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }
static float3 fetch_clamped(texture2d<float, access::read> st, int2 p, int2 mx) {
  return st.read(uint2(clamp(p, int2(0), mx))).rgb;
}
static float3 karis_avg(float3 a, float3 b, float3 c, float3 d) {
  float wa = 1.0f / (1.0f + luma(a)), wb = 1.0f / (1.0f + luma(b));
  float wc = 1.0f / (1.0f + luma(c)), wd = 1.0f / (1.0f + luma(d));
  return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-4f);
}
kernel void main0(constant BP& pc [[buffer(30)]], texture2d<float, access::read> st [[texture(0)]],
                  texture2d<float, access::read_write> di [[texture(1)]], uint2 px [[thread_position_in_grid]]) {
  if (px.x >= pc.dw || px.y >= pc.dh) return;
  int2 mx = int2(int(pc.sw) - 1, int(pc.sh) - 1);
  if (pc.mode == 0u) {
    int2 s = int2(px) * 2;
    float3 avg = karis_avg(fetch_clamped(st, s, mx), fetch_clamped(st, s + int2(1, 0), mx),
                           fetch_clamped(st, s + int2(0, 1), mx), fetch_clamped(st, s + int2(1, 1), mx));
    if (pc.th > 0.0f) {
      float lum = luma(avg);
      avg *= max(lum - pc.th, 0.0f) / max(lum, 1e-4f);
    }
    di.write(float4(avg, 1.0f), px);
  } else {
    int2 s = int2(px) / 2;
    float3 sum =
        fetch_clamped(st, s + int2(-1, -1), mx) * 1.0f + fetch_clamped(st, s + int2(0, -1), mx) * 2.0f + fetch_clamped(st, s + int2(1, -1), mx) * 1.0f +
        fetch_clamped(st, s + int2(-1,  0), mx) * 2.0f + fetch_clamped(st, s + int2(0,  0), mx) * 4.0f + fetch_clamped(st, s + int2(1,  0), mx) * 2.0f +
        fetch_clamped(st, s + int2(-1,  1), mx) * 1.0f + fetch_clamped(st, s + int2(0,  1), mx) * 2.0f + fetch_clamped(st, s + int2(1,  1), mx) * 1.0f;
    di.write(float4(di.read(px).rgb + sum * (pc.inten / 16.0f), 1.0f), px);
  }
}
)";

// =============================================================================
// 10. Async Compute 256-Bin Log-Luminance Auto-Exposure (post_exposure_hist.comp)
// =============================================================================
inline constexpr std::string_view kPostExposureHistCompHlsl = R"(
cbuffer ExposureParams : register(b0, space15) {
  uint width;
  uint height;
  uint mode;
  float min_log_lum;
  float inv_log_range;
  float dt;
  float adapt_speed;
  float target_key;
};
Texture2D<float4> hdr_tex : register(t0, space0);
RWByteAddressBuffer hist_buf : register(u1, space0);
RWByteAddressBuffer exp_buf : register(u2, space0);

groupshared uint s_hist[256];
groupshared float s_weighted[256];
groupshared uint s_counts[256];

[numthreads(16, 16, 1)]
void main(uint3 gtid : SV_DispatchThreadID, uint lidx : SV_GroupIndex) {
  if (mode == 0u) {
    s_hist[lidx] = 0u;
    GroupMemoryBarrierWithGroupSync();
    int2 px = int2(gtid.xy);
    if (px.x < int(width) && px.y < int(height)) {
      float3 hdr = hdr_tex.Load(int3(px, 0)).rgb;
      float lum = dot(hdr, float3(0.2126, 0.7152, 0.0722));
      uint bin = 0u;
      if (lum > 1e-4) {
        float t = clamp((log2(lum) - min_log_lum) * inv_log_range, 0.0, 1.0);
        bin = uint(clamp(t * 254.0 + 1.0, 1.0, 255.0));
      }
      uint prev;
      InterlockedAdd(s_hist[bin], 1u, prev);
    }
    GroupMemoryBarrierWithGroupSync();
    uint c = s_hist[lidx];
    if (c > 0u) {
      uint prev;
      hist_buf.InterlockedAdd(lidx * 4u, c, prev);
    }
  } else {
    uint count = hist_buf.Load(lidx * 4u);
    hist_buf.Store(lidx * 4u, 0u);
    s_weighted[lidx] = (lidx > 0u) ? (float(count) * float(lidx)) : 0.0;
    s_counts[lidx] = (lidx > 0u) ? count : 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
      if (lidx < stride) {
        s_weighted[lidx] += s_weighted[lidx + stride];
        s_counts[lidx] += s_counts[lidx + stride];
      }
      GroupMemoryBarrierWithGroupSync();
    }
    if (lidx == 0u) {
      uint valid = max(s_counts[0], 1u);
      float avg_bin = s_weighted[0] / float(valid);
      float log_lum = (max(avg_bin - 1.0, 0.0) / 254.0) / max(inv_log_range, 1e-5) + min_log_lum;
      float lum = clamp(exp2(log_lum), 0.01, 64.0);
      float target_exp = clamp(target_key / max(lum, 0.01), 0.15, 8.0);
      float prev_exp = asfloat(exp_buf.Load(0u));
      float prev = (prev_exp > 1e-4) ? prev_exp : target_exp;
      float k = clamp(1.0 - exp(-dt * adapt_speed), 0.0, 1.0);
      float adapted = lerp(prev, target_exp, k);
      exp_buf.Store(0u, asuint(adapted));
      exp_buf.Store(4u, asuint(lum));
      exp_buf.Store(8u, valid);
    }
  }
}
)";

inline constexpr std::string_view kPostExposureHistCompMsl = R"(
// shift_local_size(16, 16, 1)
#include <metal_stdlib>
using namespace metal;
struct EP { uint w; uint h; uint mode; float minl; float invr; float dt; float spd; float key; };
struct EB { float adapted; float lum; uint valid; uint pad; };
kernel void main0(constant EP& pc [[buffer(30)]], texture2d<float, access::read> hdr [[texture(0)]],
                  device atomic_uint* bins [[buffer(1)]], device EB* eb [[buffer(2)]],
                  uint2 px [[thread_position_in_grid]], uint lidx [[thread_index_in_threadgroup]]) {
  threadgroup atomic_uint sh[256];
  if (pc.mode == 0u) {
    atomic_store_explicit(&sh[lidx], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (px.x < pc.w && px.y < pc.h) {
      float lum = dot(hdr.read(px).rgb, float3(0.2126f, 0.7152f, 0.0722f));
      uint b = (lum > 1e-4f) ? uint(clamp(clamp((log2(lum) - pc.minl) * pc.invr, 0.0f, 1.0f) * 254.0f + 1.0f, 1.0f, 255.0f)) : 0u;
      atomic_fetch_add_explicit(&sh[b], 1u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint c = atomic_load_explicit(&sh[lidx], memory_order_relaxed);
    if (c > 0u) atomic_fetch_add_explicit(&bins[lidx], c, memory_order_relaxed);
  } else if (lidx == 0u) {
    float sw = 0.0f; uint sc = 0u;
    for (uint i = 0u; i < 256u; ++i) {
      uint c = atomic_exchange_explicit(&bins[i], 0u, memory_order_relaxed);
      if (i > 0u) { sw += float(c) * float(i); sc += c; }
    }
    uint valid = max(sc, 1u);
    float avg_bin = sw / float(valid);
    float lum = clamp(exp2((max(avg_bin - 1.0f, 0.0f) / 254.0f) / max(pc.invr, 1e-5f) + pc.minl), 0.01f, 64.0f);
    float te = clamp(pc.key / max(lum, 0.01f), 0.15f, 8.0f);
    float prev = (eb->adapted > 1e-4f) ? eb->adapted : te;
    eb->adapted = mix(prev, te, clamp(1.0f - exp(-pc.dt * pc.spd), 0.0f, 1.0f));
    eb->lum = lum; eb->valid = valid;
  }
}
)";

// =============================================================================
// 11. Async Compute McGuire Motion-Blur Tile Max (post_mblur_tile.comp)
// =============================================================================
inline constexpr std::string_view kPostMblurTileCompHlsl = R"(
cbuffer MblurTileParams : register(b0, space15) {
  uint width;
  uint height;
  uint tiles_x;
  uint tiles_y;
};
Texture2D<float2> velocity_tex : register(t0, space0);
RWTexture2D<float2> tile_max_img : register(u1, space0);

groupshared float2 s_vel[256];

[numthreads(16, 16, 1)]
void main(uint3 gtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint lidx : SV_GroupIndex) {
  int2 px = int2(gtid.xy);
  float2 v = float2(0.0, 0.0);
  if (px.x < int(width) && px.y < int(height)) v = velocity_tex.Load(int3(px, 0));
  s_vel[lidx] = v;
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (lidx < stride) {
      float2 a = s_vel[lidx], b = s_vel[lidx + stride];
      s_vel[lidx] = (dot(b, b) > dot(a, a)) ? b : a;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (lidx == 0u && gid.x < tiles_x && gid.y < tiles_y) {
    tile_max_img[int2(gid.xy)] = s_vel[0];
  }
}
)";

inline constexpr std::string_view kPostMblurTileCompMsl = R"(
// shift_local_size(16, 16, 1)
#include <metal_stdlib>
using namespace metal;
struct MP { uint w; uint h; uint tx; uint ty; };
kernel void main0(constant MP& pc [[buffer(30)]], texture2d<float, access::read> vt [[texture(0)]],
                  texture2d<float, access::write> tm [[texture(1)]],
                  uint2 px [[thread_position_in_grid]], uint2 gid [[threadgroup_position_in_grid]],
                  uint lidx [[thread_index_in_threadgroup]]) {
  threadgroup float2 sv[256];
  sv[lidx] = (px.x < pc.w && px.y < pc.h) ? vt.read(px).xy : float2(0.0f);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint s = 128u; s > 0u; s >>= 1u) {
    if (lidx < s && dot(sv[lidx+s], sv[lidx+s]) > dot(sv[lidx], sv[lidx])) sv[lidx] = sv[lidx+s];
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (lidx == 0u && gid.x < pc.tx && gid.y < pc.ty) tm.write(float4(sv[0], 0, 0), gid);
}
)";

// =============================================================================
// 12. Pre-Tonemap Linear HDR TAA-U + Exposure/Bloom + ACES (post_taa_hdr.comp)
// =============================================================================
inline constexpr std::string_view kPostTaaHdrCompHlsl = R"(
cbuffer TaaParams : register(b0, space15) {
  uint width;
  uint height;
  uint history_valid;
  float blend_alpha;
  float bloom_strength;
  float manual_exposure;
  uint bloom_width;
  uint bloom_height;
};
Texture2D<float4> curr_hdr : register(t0, space0);
Texture2D<float4> prev_history : register(t1, space0);
Texture2D<float2> velocity_tex : register(t2, space0);
Texture2D<float> depth_tex : register(t3, space0);
Texture2D<float4> bloom_tex : register(t4, space0);
ByteAddressBuffer exp_buf : register(t5, space0);
RWTexture2D<float4> out_history : register(u6, space0);
RWTexture2D<float4> out_ldr : register(u7, space0);

float3 rgb_to_ycocg(float3 c) {
  return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 ycocg_to_rgb(float3 ycc) {
  return float3(ycc.x + ycc.y - ycc.z, ycc.x + ycc.z, ycc.x - ycc.y - ycc.z);
}
float3 aces_tonemap(float3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
float3 sample_history_bilinear(float2 uv) {
  float2 fpx = uv * float2(float(width), float(height)) - float2(0.5, 0.5);
  int2 p0 = int2(floor(fpx));
  float2 f = clamp(fpx - float2(p0), 0.0, 1.0);
  int2 mx = int2(int(width) - 1, int(height) - 1);
  float3 c00 = prev_history.Load(int3(clamp(p0 + int2(0, 0), int2(0, 0), mx), 0)).rgb;
  float3 c10 = prev_history.Load(int3(clamp(p0 + int2(1, 0), int2(0, 0), mx), 0)).rgb;
  float3 c01 = prev_history.Load(int3(clamp(p0 + int2(0, 1), int2(0, 0), mx), 0)).rgb;
  float3 c11 = prev_history.Load(int3(clamp(p0 + int2(1, 1), int2(0, 0), mx), 0)).rgb;
  return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_DispatchThreadID) {
  int2 px = int2(gtid.xy);
  if (px.x >= int(width) || px.y >= int(height)) return;
  int2 mx = int2(int(width) - 1, int(height) - 1);

  float3 curr = curr_hdr.Load(int3(px, 0)).rgb;
  float3 m1 = float3(0, 0, 0), m2 = float3(0, 0, 0);
  float best_z = -1.0;
  int2 best_px = px;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      int2 sp = clamp(px + int2(dx, dy), int2(0, 0), mx);
      float3 ycc = rgb_to_ycocg(curr_hdr.Load(int3(sp, 0)).rgb);
      m1 += ycc;
      m2 += ycc * ycc;
      float z = depth_tex.Load(int3(sp, 0));
      if (z > best_z) { best_z = z; best_px = sp; }
    }
  }
  float3 mean = m1 / 9.0;
  float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, float3(0, 0, 0)));
  float3 box_min = mean - 1.25 * sigma;
  float3 box_max = mean + 1.25 * sigma;

  float2 vel = velocity_tex.Load(int3(best_px, 0));
  float2 uv = (float2(px) + 0.5) / float2(float(width), float(height));
  float2 prev_uv = uv - vel;

  float3 resolved = curr;
  if (history_valid != 0u && prev_uv.x >= 0.0 && prev_uv.x <= 1.0 && prev_uv.y >= 0.0 && prev_uv.y <= 1.0) {
    float3 hist = sample_history_bilinear(prev_uv);
    float3 hist_clamped = max(ycocg_to_rgb(clamp(rgb_to_ycocg(hist), box_min, box_max)), float3(0, 0, 0));
    float w_curr = blend_alpha / (1.0 + dot(curr, float3(0.2126, 0.7152, 0.0722)));
    float w_hist = (1.0 - blend_alpha) / (1.0 + dot(hist_clamped, float3(0.2126, 0.7152, 0.0722)));
    resolved = (curr * w_curr + hist_clamped * w_hist) / max(w_curr + w_hist, 1e-4);
  }
  out_history[px] = float4(resolved, 1.0);

  uint bw, bh;
  bloom_tex.GetDimensions(bw, bh);
  uint eff_bw = (bloom_width > 0u) ? min(bloom_width, bw) : bw;
  uint eff_bh = (bloom_height > 0u) ? min(bloom_height, bh) : bh;
  int2 bpx = clamp(int2(uv * float2(float(eff_bw), float(eff_bh))), int2(0, 0), max(int2(int(eff_bw) - 1, int(eff_bh) - 1), int2(0, 0)));
  float3 bloom = bloom_tex.Load(int3(bpx, 0)).rgb;
  float adapted = asfloat(exp_buf.Load(0u));
  float exposure = (manual_exposure > 0.0) ? manual_exposure : max(adapted, 0.15);
  float3 ldr = aces_tonemap((resolved + bloom * bloom_strength) * exposure);
  float3 srgb = pow(ldr, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
  out_ldr[px] = float4(srgb, 1.0);
}
)";

inline constexpr std::string_view kPostTaaHdrCompMsl = R"(
// shift_local_size(8, 8, 1)
#include <metal_stdlib>
using namespace metal;
struct TP { uint w; uint h; uint hv; float a; float bs; float me; uint bw; uint bh; };
struct EB { float adapted; float lum; uint valid; uint pad; };
static float3 rgb_to_ycocg(float3 c) {
  return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
                0.5f * c.r - 0.5f * c.b,
                -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}
static float3 ycocg_to_rgb(float3 ycc) {
  return float3(ycc.x + ycc.y - ycc.z, ycc.x + ycc.z, ycc.x - ycc.y - ycc.z);
}
static float3 aces_tonemap(float3 x) {
  return clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}
static float3 sample_history_bilinear(texture2d<float, access::read> ph, float2 uv, uint w, uint h) {
  float2 fpx = uv * float2(float(w), float(h)) - float2(0.5f);
  int2 p0 = int2(floor(fpx));
  float2 f = clamp(fpx - float2(p0), 0.0f, 1.0f);
  int2 mx = int2(int(w) - 1, int(h) - 1);
  float3 c00 = ph.read(uint2(clamp(p0 + int2(0, 0), int2(0), mx))).rgb;
  float3 c10 = ph.read(uint2(clamp(p0 + int2(1, 0), int2(0), mx))).rgb;
  float3 c01 = ph.read(uint2(clamp(p0 + int2(0, 1), int2(0), mx))).rgb;
  float3 c11 = ph.read(uint2(clamp(p0 + int2(1, 1), int2(0), mx))).rgb;
  return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}
kernel void main0(constant TP& pc [[buffer(30)]], texture2d<float, access::read> ch [[texture(0)]],
                  texture2d<float, access::read> ph [[texture(1)]], texture2d<float, access::read> vt [[texture(2)]],
                  texture2d<float, access::read> dt [[texture(3)]], texture2d<float, access::read> bt [[texture(4)]],
                  device const EB* eb [[buffer(5)]], texture2d<float, access::write> oh [[texture(6)]],
                  texture2d<float, access::write> ol [[texture(7)]], uint2 px [[thread_position_in_grid]]) {
  if (px.x >= pc.w || px.y >= pc.h) return;
  int2 ipx = int2(px);
  int2 mx = int2(int(pc.w) - 1, int(pc.h) - 1);
  float3 curr = ch.read(px).rgb;
  float3 m1 = float3(0.0f), m2 = float3(0.0f);
  float best_z = -1.0f;
  int2 best_px = ipx;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      int2 sp = clamp(ipx + int2(dx, dy), int2(0), mx);
      float3 ycc = rgb_to_ycocg(ch.read(uint2(sp)).rgb);
      m1 += ycc;
      m2 += ycc * ycc;
      float z = dt.read(uint2(sp)).x;
      if (z > best_z) { best_z = z; best_px = sp; }
    }
  }
  float3 mean = m1 / 9.0f;
  float3 sigma = sqrt(max(m2 / 9.0f - mean * mean, float3(0.0f)));
  float3 box_min = mean - 1.25f * sigma;
  float3 box_max = mean + 1.25f * sigma;

  float2 vel = vt.read(uint2(best_px)).xy;
  float2 uv = (float2(px) + 0.5f) / float2(float(pc.w), float(pc.h));
  float2 prev_uv = uv - vel;

  float3 resolved = curr;
  if (pc.hv != 0u && prev_uv.x >= 0.0f && prev_uv.x <= 1.0f && prev_uv.y >= 0.0f && prev_uv.y <= 1.0f) {
    float3 hist = sample_history_bilinear(ph, prev_uv, pc.w, pc.h);
    float3 hist_clamped = max(ycocg_to_rgb(clamp(rgb_to_ycocg(hist), box_min, box_max)), float3(0.0f));
    float w_curr = pc.a / (1.0f + dot(curr, float3(0.2126f, 0.7152f, 0.0722f)));
    float w_hist = (1.0f - pc.a) / (1.0f + dot(hist_clamped, float3(0.2126f, 0.7152f, 0.0722f)));
    resolved = (curr * w_curr + hist_clamped * w_hist) / max(w_curr + w_hist, 1e-4f);
  }
  oh.write(float4(resolved, 1.0f), px);

  uint eff_bw = (pc.bw > 0u) ? min(pc.bw, uint(bt.get_width())) : uint(bt.get_width());
  uint eff_bh = (pc.bh > 0u) ? min(pc.bh, uint(bt.get_height())) : uint(bt.get_height());
  uint2 bpx = clamp(uint2(uv * float2(float(eff_bw), float(eff_bh))), uint2(0u), uint2(max(eff_bw, 1u) - 1u, max(eff_bh, 1u) - 1u));
  float3 bloom = bt.read(bpx).rgb;
  float expv = (pc.me > 0.0f) ? pc.me : max(eb->adapted, 0.15f);
  float3 ldr = aces_tonemap((resolved + bloom * pc.bs) * expv);
  ol.write(float4(pow(ldr, float3(1.0f / 2.2f)), 1.0f), px);
}
)";

}  // namespace shift::rhi::shaders
