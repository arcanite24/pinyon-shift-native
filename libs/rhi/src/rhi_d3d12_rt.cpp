#include "rhi_d3d12_internal.h"

#include <cstdio>
#include <vector>

namespace shift::rhi::d3d12 {

namespace {

std::string hr_err(const char* what, HRESULT hr) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
  return std::string(what) + " failed (HRESULT " + buf + ")";
}

#ifdef SHIFT_ENABLE_RAY_TRACING
bool build_dxr_triangle_geometries(const D3D12DeviceImpl& device,
                                   std::span<const BlasTriangleGeometryDesc> descs,
                                   bool need_addresses,
                                   std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& out_geoms) {
  out_geoms.clear();
  out_geoms.reserve(descs.size());

  for (const BlasTriangleGeometryDesc& g : descs) {
    if (g.primitive_count == 0) continue;
    D3D12_RAYTRACING_GEOMETRY_DESC gd{};
    gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    gd.Flags = to_d3d12_geometry_flags(g.flags);

    auto& tri = gd.Triangles;
    tri.Transform3x4 = 0;
    tri.VertexFormat = to_dxgi_format(g.vertex_format);
    tri.VertexCount = g.max_vertex + 1u;
    tri.VertexBuffer.StrideInBytes = g.vertex_stride;

    if (g.index_buffer) {
      tri.IndexFormat = (g.index_type == IndexType::Uint16) ? DXGI_FORMAT_R16_UINT
                                                            : DXGI_FORMAT_R32_UINT;
      tri.IndexCount = g.primitive_count * 3u;
    } else {
      tri.IndexFormat = DXGI_FORMAT_UNKNOWN;
      tri.IndexCount = 0;
    }

    if (need_addresses) {
      const uint64_t vb_va = device.buffer_device_address(g.vertex_buffer);
      if (vb_va == 0) return false;
      tri.VertexBuffer.StartAddress = vb_va + g.vertex_offset;
      if (g.index_buffer) {
        const uint64_t ib_va = device.buffer_device_address(g.index_buffer);
        if (ib_va == 0) return false;
        tri.IndexBuffer = ib_va + g.index_offset;
      }
    }

    out_geoms.push_back(gd);
  }
  return !out_geoms.empty();
}
#endif

}  // namespace

// -----------------------------------------------------------------------------
// DXR 1.1 Prebuild Size Queries
// -----------------------------------------------------------------------------
bool D3D12DeviceImpl::query_blas_sizes(std::span<const BlasTriangleGeometryDesc> geometries,
                                       AccelStructBuildFlags flags,
                                       AccelStructBuildSizes& out_sizes) const {
  out_sizes = {};
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !device_) return false;
  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> dxr_geoms;
  if (!build_dxr_triangle_geometries(*this, geometries, false, dxr_geoms)) return false;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = to_d3d12_as_build_flags(flags);
  inputs.NumDescs = static_cast<UINT>(dxr_geoms.size());
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.pGeometryDescs = dxr_geoms.data();

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  out_sizes.accel_struct_size = info.ResultDataMaxSizeInBytes;
  out_sizes.build_scratch_size = info.ScratchDataSizeInBytes;
  out_sizes.update_scratch_size = info.UpdateScratchDataSizeInBytes;
  return out_sizes.accel_struct_size > 0;
#else
  (void)geometries;
  (void)flags;
  return false;
#endif
}

bool D3D12DeviceImpl::query_tlas_sizes(uint32_t max_instances, AccelStructBuildFlags flags,
                                       AccelStructBuildSizes& out_sizes) const {
  out_sizes = {};
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !device_) return false;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.Flags = to_d3d12_as_build_flags(flags);
  inputs.NumDescs = max_instances;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  out_sizes.accel_struct_size = info.ResultDataMaxSizeInBytes;
  out_sizes.build_scratch_size = info.ScratchDataSizeInBytes;
  out_sizes.update_scratch_size = info.UpdateScratchDataSizeInBytes;
  return out_sizes.accel_struct_size > 0;
#else
  (void)max_instances;
  (void)flags;
  return false;
#endif
}

// -----------------------------------------------------------------------------
// BLAS / TLAS Storage Allocation & Destruction
// -----------------------------------------------------------------------------
AccelStructHandle D3D12DeviceImpl::create_accel_struct_impl(const AccelStructDesc& desc,
                                                            std::string& error) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !device_) {
    error = "DXR 1.1 acceleration structures are not supported on this device";
    return {};
  }
  if (desc.size_bytes == 0) {
    error = "create_accel_struct: size_bytes must be > 0";
    return {};
  }

  const uint64_t alloc_size = align_up(
      desc.size_bytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = alloc_size;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  AccelStructEntry entry{};
  const HRESULT hr = device_->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
      nullptr, IID_PPV_ARGS(&entry.resource));
  if (FAILED(hr)) {
    error = hr_err("CreateCommittedResource(accel_struct)", hr);
    return {};
  }

  entry.size = alloc_size;
  entry.gpu_va = entry.resource->GetGPUVirtualAddress();
  entry.kind = desc.kind;

  ++live_alloc_count_;
  live_alloc_bytes_ += alloc_size;

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return accel_structs_.allocate(std::move(entry));
#else
  (void)desc;
  error = "ray tracing support is disabled at compile time (SHIFT_ENABLE_RAY_TRACING=OFF)";
  return {};
#endif
}

AccelStructHandle D3D12DeviceImpl::create_blas(const AccelStructDesc& desc, std::string& error) {
  AccelStructDesc d = desc;
  d.kind = AccelStructKind::BottomLevel;
  return create_accel_struct_impl(d, error);
}

AccelStructHandle D3D12DeviceImpl::create_tlas(const AccelStructDesc& desc, std::string& error) {
  AccelStructDesc d = desc;
  d.kind = AccelStructKind::TopLevel;
  return create_accel_struct_impl(d, error);
}

void D3D12DeviceImpl::destroy_accel_struct(AccelStructHandle handle) {
  AccelStructEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!accel_structs_.remove(handle, &entry)) return;
  }
  if (entry.resource) {
    --live_alloc_count_;
    live_alloc_bytes_ -= entry.size;
    entry.resource.Reset();
  }
}

uint64_t D3D12DeviceImpl::accel_struct_device_address(AccelStructHandle handle) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const AccelStructEntry* e = accel_structs_.get(handle);
  return e ? e->gpu_va : 0ull;
}

// -----------------------------------------------------------------------------
// DXR 1.1 Command Recording: BLAS Build, Compacted Size Query, Compact Copy, TLAS Build
// -----------------------------------------------------------------------------
void D3D12DeviceImpl::cmd_build_blas_impl(ID3D12GraphicsCommandList4* cmd,
                                          const BlasBuildDesc& desc) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !cmd) return;
  const AccelStructEntry* dst_as = lookup_accel_struct(desc.dst);
  const uint64_t scratch_va = buffer_device_address(desc.scratch_buffer);
  if (!dst_as || dst_as->gpu_va == 0 || scratch_va == 0) return;

  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> dxr_geoms;
  if (!build_dxr_triangle_geometries(*this, desc.geometries, true, dxr_geoms)) return;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.DestAccelerationStructureData = dst_as->gpu_va;
  build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  build.Inputs.Flags = to_d3d12_as_build_flags(desc.flags);
  build.Inputs.NumDescs = static_cast<UINT>(dxr_geoms.size());
  build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  build.Inputs.pGeometryDescs = dxr_geoms.data();
  build.ScratchAccelerationStructureData = scratch_va + desc.scratch_offset;

  cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
#else
  (void)cmd;
  (void)desc;
#endif
}

void D3D12DeviceImpl::cmd_write_blas_compacted_size_impl(ID3D12GraphicsCommandList4* cmd,
                                                         AccelStructHandle blas,
                                                         QueryPoolHandle pool,
                                                         uint32_t query_index) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !cmd) return;
  const AccelStructEntry* as = lookup_accel_struct(blas);
  const QueryPoolEntry* qp = lookup_query_pool(pool);
  if (!as || as->gpu_va == 0 || !qp || qp->type != QueryType::AccelStructCompactedSize ||
      !qp->gpu_buffer || !qp->readback || query_index >= qp->count) {
    return;
  }

  const uint64_t byte_off = static_cast<uint64_t>(query_index) * sizeof(uint64_t);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuild{};
  postbuild.DestBuffer = qp->gpu_buffer->GetGPUVirtualAddress() + byte_off;
  postbuild.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;

  D3D12_GPU_VIRTUAL_ADDRESS src_va = as->gpu_va;
  cmd->EmitRaytracingAccelerationStructurePostbuildInfo(&postbuild, 1, &src_va);

  // Transition postbuild UAV buffer -> COPY_SOURCE, copy 8 bytes to readback, restore UAV.
  D3D12_RESOURCE_BARRIER to_src{};
  to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_src.Transition.pResource = qp->gpu_buffer.Get();
  to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cmd->ResourceBarrier(1, &to_src);

  cmd->CopyBufferRegion(qp->readback.Get(), byte_off, qp->gpu_buffer.Get(), byte_off, sizeof(uint64_t));

  D3D12_RESOURCE_BARRIER to_uav{};
  to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_uav.Transition.pResource = qp->gpu_buffer.Get();
  to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  cmd->ResourceBarrier(1, &to_uav);
#else
  (void)cmd;
  (void)blas;
  (void)pool;
  (void)query_index;
#endif
}

void D3D12DeviceImpl::cmd_copy_compact_blas_impl(ID3D12GraphicsCommandList4* cmd,
                                                 AccelStructHandle src, AccelStructHandle dst) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !cmd) return;
  const AccelStructEntry* s = lookup_accel_struct(src);
  const AccelStructEntry* d = lookup_accel_struct(dst);
  if (!s || !d || s->gpu_va == 0 || d->gpu_va == 0) return;

  cmd->CopyRaytracingAccelerationStructure(
      d->gpu_va, s->gpu_va, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
#else
  (void)cmd;
  (void)src;
  (void)dst;
#endif
}

void D3D12DeviceImpl::cmd_build_tlas_impl(ID3D12GraphicsCommandList4* cmd,
                                          const TlasBuildDesc& desc) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !cmd) return;
  const AccelStructEntry* dst_as = lookup_accel_struct(desc.dst);
  const uint64_t scratch_va = buffer_device_address(desc.scratch_buffer);
  const uint64_t inst_va = desc.instance_count > 0 ? buffer_device_address(desc.instance_buffer) : 0ull;
  if (!dst_as || dst_as->gpu_va == 0 || scratch_va == 0) return;
  if (desc.instance_count > 0 && inst_va == 0) return;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.DestAccelerationStructureData = dst_as->gpu_va;
  build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  build.Inputs.Flags = to_d3d12_as_build_flags(desc.flags);
  build.Inputs.NumDescs = desc.instance_count;
  build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  build.Inputs.InstanceDescs = desc.instance_count > 0 ? (inst_va + desc.instance_offset) : 0ull;
  build.ScratchAccelerationStructureData = scratch_va + desc.scratch_offset;

  cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
#else
  (void)cmd;
  (void)desc;
#endif
}

}  // namespace shift::rhi::d3d12
