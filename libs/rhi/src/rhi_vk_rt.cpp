#include "rhi_vk_internal.h"

#include <vector>

namespace shift::rhi::vk {

namespace {

std::string vk_err(const char* what, VkResult r) {
  return std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")";
}

#ifdef SHIFT_ENABLE_RAY_TRACING
bool build_triangle_geometries(const VkDeviceImpl& device,
                               std::span<const BlasTriangleGeometryDesc> descs,
                               bool need_device_addresses,
                               std::vector<VkAccelerationStructureGeometryKHR>& out_geoms,
                               std::vector<uint32_t>& out_prim_counts,
                               std::vector<VkAccelerationStructureBuildRangeInfoKHR>& out_ranges) {
  out_geoms.clear();
  out_prim_counts.clear();
  out_ranges.clear();
  out_geoms.reserve(descs.size());
  out_prim_counts.reserve(descs.size());
  out_ranges.reserve(descs.size());

  for (const BlasTriangleGeometryDesc& g : descs) {
    if (g.primitive_count == 0) continue;
    VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    ag.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    ag.flags = to_vk_geometry_flags(g.flags);

    auto& tri = ag.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = to_vk_format(g.vertex_format);
    tri.vertexStride = g.vertex_stride;
    tri.maxVertex = g.max_vertex;
    tri.indexType = g.index_buffer
                        ? (g.index_type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32)
                        : VK_INDEX_TYPE_NONE_KHR;

    if (need_device_addresses) {
      const uint64_t vb_addr = device.buffer_device_address(g.vertex_buffer);
      if (vb_addr == 0) return false;
      tri.vertexData.deviceAddress = vb_addr + g.vertex_offset;
      if (g.index_buffer) {
        const uint64_t ib_addr = device.buffer_device_address(g.index_buffer);
        if (ib_addr == 0) return false;
        tri.indexData.deviceAddress = ib_addr + g.index_offset;
      }
    }

    out_geoms.push_back(ag);
    out_prim_counts.push_back(g.primitive_count);
    out_ranges.push_back({g.primitive_count, 0u, 0u, 0u});
  }
  return !out_geoms.empty();
}
#endif

}  // namespace

// -----------------------------------------------------------------------------
// Size queries
// -----------------------------------------------------------------------------
bool VkDeviceImpl::query_blas_sizes(std::span<const BlasTriangleGeometryDesc> geometries,
                                    AccelStructBuildFlags flags,
                                    AccelStructBuildSizes& out_sizes) const {
  out_sizes = {};
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkGetAccelerationStructureBuildSizesKHR) return false;
  std::vector<VkAccelerationStructureGeometryKHR> vk_geoms;
  std::vector<uint32_t> prim_counts;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  if (!build_triangle_geometries(*this, geometries, false, vk_geoms, prim_counts, ranges)) return false;

  VkAccelerationStructureBuildGeometryInfoKHR bgi{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  bgi.flags = to_vk_as_build_flags(flags);
  bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  bgi.geometryCount = static_cast<uint32_t>(vk_geoms.size());
  bgi.pGeometries = vk_geoms.data();

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                          &bgi, prim_counts.data(), &sizes);
  out_sizes.accel_struct_size = sizes.accelerationStructureSize;
  out_sizes.build_scratch_size = sizes.buildScratchSize;
  out_sizes.update_scratch_size = sizes.updateScratchSize;
  return out_sizes.accel_struct_size > 0;
#else
  (void)geometries;
  (void)flags;
  return false;
#endif
}

bool VkDeviceImpl::query_tlas_sizes(uint32_t max_instances, AccelStructBuildFlags flags,
                                    AccelStructBuildSizes& out_sizes) const {
  out_sizes = {};
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkGetAccelerationStructureBuildSizesKHR) return false;

  VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  ag.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  ag.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  ag.geometry.instances.arrayOfPointers = VK_FALSE;

  VkAccelerationStructureBuildGeometryInfoKHR bgi{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  bgi.flags = to_vk_as_build_flags(flags);
  bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  bgi.geometryCount = 1;
  bgi.pGeometries = &ag;

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                          &bgi, &max_instances, &sizes);
  out_sizes.accel_struct_size = sizes.accelerationStructureSize;
  out_sizes.build_scratch_size = sizes.buildScratchSize;
  out_sizes.update_scratch_size = sizes.updateScratchSize;
  return out_sizes.accel_struct_size > 0;
#else
  (void)max_instances;
  (void)flags;
  return false;
#endif
}

// -----------------------------------------------------------------------------
// Creation & destruction
// -----------------------------------------------------------------------------
AccelStructHandle VkDeviceImpl::create_accel_struct_impl(const AccelStructDesc& desc, std::string& error) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkCreateAccelerationStructureKHR || !vkGetAccelerationStructureDeviceAddressKHR) {
    error = "ray tracing acceleration structures are not supported on this device";
    return {};
  }
  if (desc.size_bytes == 0) {
    error = "create_accel_struct: size_bytes must be > 0";
    return {};
  }

  BufferDesc bdesc{};
  bdesc.size = desc.size_bytes;
  bdesc.usage = BufferUsage::AccelStructStorage | BufferUsage::DeviceAddress;
  bdesc.memory = MemoryHint::GpuOnly;
  bdesc.debug_name = desc.debug_name;
  BufferHandle storage = create_buffer(bdesc, error);
  if (!storage) return {};

  const BufferEntry* be = lookup_buffer(storage);
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = be->buffer;
  ci.size = desc.size_bytes;
  ci.type = desc.kind == AccelStructKind::TopLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                                   : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

  AccelStructEntry entry{};
  entry.storage_buffer = storage;
  entry.size = desc.size_bytes;
  entry.kind = desc.kind;
  const VkResult r = vkCreateAccelerationStructureKHR(device_, &ci, nullptr, &entry.handle);
  if (r != VK_SUCCESS) {
    destroy_buffer(storage);
    error = vk_err("vkCreateAccelerationStructureKHR", r);
    return {};
  }

  VkAccelerationStructureDeviceAddressInfoKHR dai{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  dai.accelerationStructure = entry.handle;
  entry.device_address = vkGetAccelerationStructureDeviceAddressKHR(device_, &dai);

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return accel_structs_.allocate(std::move(entry));
#else
  (void)desc;
  error = "ray tracing support is disabled at compile time (SHIFT_ENABLE_RAY_TRACING=OFF)";
  return {};
#endif
}

AccelStructHandle VkDeviceImpl::create_blas(const AccelStructDesc& desc, std::string& error) {
  AccelStructDesc d = desc;
  d.kind = AccelStructKind::BottomLevel;
  return create_accel_struct_impl(d, error);
}

AccelStructHandle VkDeviceImpl::create_tlas(const AccelStructDesc& desc, std::string& error) {
  AccelStructDesc d = desc;
  d.kind = AccelStructKind::TopLevel;
  return create_accel_struct_impl(d, error);
}

void VkDeviceImpl::destroy_accel_struct(AccelStructHandle handle) {
  AccelStructEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!accel_structs_.remove(handle, &entry)) return;
  }
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (entry.handle && vkDestroyAccelerationStructureKHR) {
    vkDestroyAccelerationStructureKHR(device_, entry.handle, nullptr);
  }
#endif
  if (entry.storage_buffer) {
    destroy_buffer(entry.storage_buffer);
  }
}

uint64_t VkDeviceImpl::accel_struct_device_address(AccelStructHandle handle) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const AccelStructEntry* e = accel_structs_.get(handle);
  return e ? e->device_address : 0ull;
}

// -----------------------------------------------------------------------------
// Command buffer RT build & compaction commands
// -----------------------------------------------------------------------------
void VkDeviceImpl::cmd_build_blas_impl(VkCommandBuffer cmd, const BlasBuildDesc& desc) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkCmdBuildAccelerationStructuresKHR) return;
  const AccelStructEntry* dst_as = lookup_accel_struct(desc.dst);
  const uint64_t scratch_addr = buffer_device_address(desc.scratch_buffer);
  if (!dst_as || !dst_as->handle || scratch_addr == 0) return;

  std::vector<VkAccelerationStructureGeometryKHR> vk_geoms;
  std::vector<uint32_t> prim_counts;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  if (!build_triangle_geometries(*this, desc.geometries, true, vk_geoms, prim_counts, ranges)) return;

  VkAccelerationStructureBuildGeometryInfoKHR bgi{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  bgi.flags = to_vk_as_build_flags(desc.flags);
  bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  bgi.dstAccelerationStructure = dst_as->handle;
  bgi.geometryCount = static_cast<uint32_t>(vk_geoms.size());
  bgi.pGeometries = vk_geoms.data();
  bgi.scratchData.deviceAddress = scratch_addr + desc.scratch_offset;

  const VkAccelerationStructureBuildRangeInfoKHR* p_ranges = ranges.data();
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &p_ranges);
#else
  (void)cmd;
  (void)desc;
#endif
}

void VkDeviceImpl::cmd_write_blas_compacted_size_impl(VkCommandBuffer cmd, AccelStructHandle blas,
                                                      QueryPoolHandle pool, uint32_t query_index) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkCmdWriteAccelerationStructuresPropertiesKHR) return;
  const AccelStructEntry* as = lookup_accel_struct(blas);
  const QueryPoolEntry* qp = lookup_query_pool(pool);
  if (!as || !as->handle || !qp || query_index >= qp->count) return;
  vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &as->handle,
                                                VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                qp->pool, query_index);
#else
  (void)cmd;
  (void)blas;
  (void)pool;
  (void)query_index;
#endif
}

void VkDeviceImpl::cmd_copy_compact_blas_impl(VkCommandBuffer cmd, AccelStructHandle src, AccelStructHandle dst) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkCmdCopyAccelerationStructureKHR) return;
  const AccelStructEntry* s = lookup_accel_struct(src);
  const AccelStructEntry* d = lookup_accel_struct(dst);
  if (!s || !s->handle || !d || !d->handle) return;

  VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
  copy.src = s->handle;
  copy.dst = d->handle;
  copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
  vkCmdCopyAccelerationStructureKHR(cmd, &copy);
#else
  (void)cmd;
  (void)src;
  (void)dst;
#endif
}

void VkDeviceImpl::cmd_build_tlas_impl(VkCommandBuffer cmd, const TlasBuildDesc& desc) {
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (!caps_.ray_query || !vkCmdBuildAccelerationStructuresKHR) return;
  const AccelStructEntry* dst_as = lookup_accel_struct(desc.dst);
  const uint64_t scratch_addr = buffer_device_address(desc.scratch_buffer);
  const uint64_t inst_addr = desc.instance_count > 0 ? buffer_device_address(desc.instance_buffer) : 0ull;
  if (!dst_as || !dst_as->handle || scratch_addr == 0) return;
  if (desc.instance_count > 0 && inst_addr == 0) return;

  VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  ag.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  ag.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  ag.geometry.instances.arrayOfPointers = VK_FALSE;
  ag.geometry.instances.data.deviceAddress = desc.instance_count > 0 ? (inst_addr + desc.instance_offset) : 0ull;

  VkAccelerationStructureBuildGeometryInfoKHR bgi{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  bgi.flags = to_vk_as_build_flags(desc.flags);
  bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  bgi.dstAccelerationStructure = dst_as->handle;
  bgi.geometryCount = 1;
  bgi.pGeometries = &ag;
  bgi.scratchData.deviceAddress = scratch_addr + desc.scratch_offset;

  VkAccelerationStructureBuildRangeInfoKHR range{desc.instance_count, 0u, 0u, 0u};
  const VkAccelerationStructureBuildRangeInfoKHR* p_range = &range;
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &p_range);
#else
  (void)cmd;
  (void)desc;
#endif
}

}  // namespace shift::rhi::vk
