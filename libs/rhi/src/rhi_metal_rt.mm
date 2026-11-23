#include "rhi_metal_internal.h"

#include <cstring>
#include <unordered_map>

namespace shift::rhi::mtl {

namespace {

MTLAccelerationStructureInstanceOptions to_mtl_instance_options(uint32_t flags_8) noexcept {
  MTLAccelerationStructureInstanceOptions opts = MTLAccelerationStructureInstanceOptionNone;
  if (flags_8 & (1u << 0)) opts |= MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
  if (flags_8 & (1u << 1)) {
    opts |= MTLAccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise;
  }
  if (flags_8 & (1u << 2)) opts |= MTLAccelerationStructureInstanceOptionOpaque;
  if (flags_8 & (1u << 3)) opts |= MTLAccelerationStructureInstanceOptionNonOpaque;
  return opts;
}

MTLPrimitiveAccelerationStructureDescriptor* build_prim_descriptor(
    const MetalDeviceImpl& device,
    std::span<const BlasTriangleGeometryDesc> geometries,
    AccelStructBuildFlags flags,
    bool require_buffers) {
  NSMutableArray<MTLAccelerationStructureGeometryDescriptor*>* array =
      [NSMutableArray arrayWithCapacity:geometries.size()];

  for (const BlasTriangleGeometryDesc& g : geometries) {
    if (g.primitive_count == 0) continue;
    MTLAccelerationStructureTriangleGeometryDescriptor* tri =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    tri.triangleCount = g.primitive_count;
    tri.vertexStride = g.vertex_stride;
    tri.vertexFormat = to_mtl_attribute_format(g.vertex_format);
    tri.indexType = (g.index_type == IndexType::Uint16) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    tri.opaque = has_flag(g.flags, GeometryFlags::Opaque) ? YES : NO;
    tri.allowDuplicateIntersectionFunctionInvocation =
        has_flag(g.flags, GeometryFlags::NoDuplicateAnyHitInvocation) ? NO : YES;

    if (g.vertex_buffer) {
      BufferEntry vb{};
      if (device.lookup_buffer_copy(g.vertex_buffer, vb) && vb.buffer) {
        tri.vertexBuffer = vb.buffer;
        tri.vertexBufferOffset = g.vertex_offset;
      } else if (require_buffers) {
        return nil;
      }
    } else if (require_buffers) {
      return nil;
    }

    if (g.index_buffer) {
      BufferEntry ib{};
      if (device.lookup_buffer_copy(g.index_buffer, ib) && ib.buffer) {
        tri.indexBuffer = ib.buffer;
        tri.indexBufferOffset = g.index_offset;
      } else if (require_buffers) {
        return nil;
      }
    }

    [array addObject:tri];
  }

  if ([array count] == 0) return nil;

  MTLPrimitiveAccelerationStructureDescriptor* desc =
      [MTLPrimitiveAccelerationStructureDescriptor descriptor];
  desc.geometryDescriptors = array;
  desc.usage = to_mtl_as_usage(flags);
  return desc;
}

}  // namespace

// -----------------------------------------------------------------------------
// Size Queries
// -----------------------------------------------------------------------------
bool MetalDeviceImpl::query_blas_sizes(std::span<const BlasTriangleGeometryDesc> geometries,
                                       AccelStructBuildFlags flags,
                                       AccelStructBuildSizes& out_sizes) const {
  out_sizes = {};
  if (!caps_.ray_query || !device_) return false;

  @autoreleasepool {
    MTLPrimitiveAccelerationStructureDescriptor* prim =
        build_prim_descriptor(*this, geometries, flags, false);
    if (!prim) return false;
    MTLAccelerationStructureSizes sz = [device_ accelerationStructureSizesWithDescriptor:prim];
    out_sizes.accel_struct_size = sz.accelerationStructureSize;
    out_sizes.build_scratch_size = sz.buildScratchBufferSize;
    out_sizes.update_scratch_size = sz.refitScratchBufferSize;
    return out_sizes.accel_struct_size > 0;
  }
}

bool MetalDeviceImpl::query_tlas_sizes(uint32_t max_instances, AccelStructBuildFlags flags,
                                       AccelStructBuildSizes& out_sizes) const {
  out_sizes = {};
  if (!caps_.ray_query || !device_) return false;

  @autoreleasepool {
    MTLInstanceAccelerationStructureDescriptor* desc =
        [MTLInstanceAccelerationStructureDescriptor descriptor];
    desc.instanceCount = max_instances;
    desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    desc.instanceDescriptorStride = sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
    desc.usage = to_mtl_as_usage(flags);

    MTLAccelerationStructureSizes sz = [device_ accelerationStructureSizesWithDescriptor:desc];
    out_sizes.accel_struct_size = sz.accelerationStructureSize;
    out_sizes.build_scratch_size = sz.buildScratchBufferSize;
    out_sizes.update_scratch_size = sz.refitScratchBufferSize;
    return out_sizes.accel_struct_size > 0;
  }
}

// -----------------------------------------------------------------------------
// Creation & Destruction
// -----------------------------------------------------------------------------
AccelStructHandle MetalDeviceImpl::create_accel_struct_impl(const AccelStructDesc& desc,
                                                            std::string& error) {
  if (!caps_.ray_query || !device_) {
    error = "Metal hardware ray tracing is not supported on this device";
    return {};
  }
  if (desc.size_bytes == 0) {
    error = "create_accel_struct(Metal): size_bytes must be > 0";
    return {};
  }

  @autoreleasepool {
    id<MTLAccelerationStructure> as = [device_ newAccelerationStructureWithSize:desc.size_bytes];
    if (!as) {
      error = "MTLDevice newAccelerationStructureWithSize failed";
      return {};
    }
    if (desc.debug_name && *desc.debug_name) {
      as.label = [NSString stringWithUTF8String:desc.debug_name];
    }

    const uint64_t gpu_id = static_cast<uint64_t>([as gpuResourceID]._impl);
    AccelStructEntry entry{};
    entry.handle = as;
    entry.size = desc.size_bytes;
    entry.device_address = gpu_id;
    entry.kind = desc.kind;

    std::lock_guard<std::mutex> lock(resource_mutex_);
    AccelStructHandle h = accel_structs_.allocate(std::move(entry));
    if (gpu_id != 0) as_address_map_[gpu_id] = h;
    ++live_alloc_count_;
    live_alloc_bytes_ += desc.size_bytes;
    return h;
  }
}

AccelStructHandle MetalDeviceImpl::create_blas(const AccelStructDesc& desc, std::string& error) {
  AccelStructDesc d = desc;
  d.kind = AccelStructKind::BottomLevel;
  return create_accel_struct_impl(d, error);
}

AccelStructHandle MetalDeviceImpl::create_tlas(const AccelStructDesc& desc, std::string& error) {
  AccelStructDesc d = desc;
  d.kind = AccelStructKind::TopLevel;
  return create_accel_struct_impl(d, error);
}

void MetalDeviceImpl::destroy_accel_struct(AccelStructHandle handle) {
  AccelStructEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!accel_structs_.remove(handle, &entry)) return;
    if (entry.device_address != 0) as_address_map_.erase(entry.device_address);
    if (live_alloc_count_ > 0) --live_alloc_count_;
    live_alloc_bytes_ =
        (live_alloc_bytes_ >= entry.size) ? (live_alloc_bytes_ - entry.size) : 0;
  }
  entry.instanced_blases = nil;
  entry.handle = nil;
}

uint64_t MetalDeviceImpl::accel_struct_device_address(AccelStructHandle handle) const {
  AccelStructEntry e{};
  if (!lookup_accel_struct_copy(handle, e)) return 0ull;
  return e.device_address;
}

// -----------------------------------------------------------------------------
// Command Buffer RT Build & Compaction Commands
// -----------------------------------------------------------------------------
void MetalDeviceImpl::cmd_build_blas_impl(MetalCommandListImpl& cmd, const BlasBuildDesc& desc) {
  if (!caps_.ray_query) return;
  AccelStructEntry dst_as{};
  BufferEntry scratch{};
  if (!lookup_accel_struct_copy(desc.dst, dst_as) || !dst_as.handle) return;
  if (!lookup_buffer_copy(desc.scratch_buffer, scratch) || !scratch.buffer) return;

  @autoreleasepool {
    MTLPrimitiveAccelerationStructureDescriptor* prim =
        build_prim_descriptor(*this, desc.geometries, desc.flags, true);
    if (!prim) return;

    id<MTLAccelerationStructureCommandEncoder> enc = cmd.ensure_accel_encoder();
    if (!enc) return;
    [enc buildAccelerationStructure:dst_as.handle
                         descriptor:prim
                      scratchBuffer:scratch.buffer
                scratchBufferOffset:desc.scratch_offset];
  }
}

void MetalDeviceImpl::cmd_write_blas_compacted_size_impl(MetalCommandListImpl& cmd,
                                                         AccelStructHandle blas,
                                                         QueryPoolHandle pool,
                                                         uint32_t query_index) {
  if (!caps_.ray_query) return;
  AccelStructEntry as{};
  QueryPoolEntry qp{};
  if (!lookup_accel_struct_copy(blas, as) || !as.handle) return;
  if (!lookup_query_pool_copy(pool, qp) || qp.type != QueryType::AccelStructCompactedSize) return;
  if (!qp.result_buffer || query_index >= qp.count) return;

  id<MTLAccelerationStructureCommandEncoder> enc = cmd.ensure_accel_encoder();
  if (!enc) return;
  [enc writeCompactedAccelerationStructureSize:as.handle
                                      toBuffer:qp.result_buffer
                                        offset:static_cast<NSUInteger>(query_index) * sizeof(uint64_t)
                                  sizeDataType:MTLDataTypeULong];
}

void MetalDeviceImpl::cmd_copy_compact_blas_impl(MetalCommandListImpl& cmd,
                                                 AccelStructHandle src,
                                                 AccelStructHandle dst) {
  if (!caps_.ray_query) return;
  AccelStructEntry s{}, d{};
  if (!lookup_accel_struct_copy(src, s) || !s.handle) return;
  if (!lookup_accel_struct_copy(dst, d) || !d.handle) return;

  id<MTLAccelerationStructureCommandEncoder> enc = cmd.ensure_accel_encoder();
  if (!enc) return;
  [enc copyAndCompactAccelerationStructure:s.handle toAccelerationStructure:d.handle];
}

void MetalDeviceImpl::cmd_build_tlas_impl(MetalCommandListImpl& cmd, const TlasBuildDesc& desc) {
  if (!caps_.ray_query) return;
  AccelStructEntry dst_as{};
  BufferEntry scratch{};
  if (!lookup_accel_struct_copy(desc.dst, dst_as) || !dst_as.handle) return;
  if (!lookup_buffer_copy(desc.scratch_buffer, scratch) || !scratch.buffer) return;

  @autoreleasepool {
    NSMutableArray<id<MTLAccelerationStructure>>* blas_list = [NSMutableArray array];
    id<MTLBuffer> mtl_inst_buf = nil;
    uint32_t valid_instances = 0;

    if (desc.instance_count > 0) {
      BufferEntry src_inst{};
      if (!lookup_buffer_copy(desc.instance_buffer, src_inst) || !src_inst.buffer) return;
      if ([src_inst.buffer storageMode] == MTLStorageModePrivate) return;

      const size_t mtl_bytes =
          static_cast<size_t>(desc.instance_count) *
          sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
      mtl_inst_buf = [device_ newBufferWithLength:mtl_bytes options:MTLResourceStorageModeShared];
      if (!mtl_inst_buf) return;
      cmd.keep_transient_buffer(mtl_inst_buf);

      const auto* src_ptr = reinterpret_cast<const TlasInstanceDesc*>(
          static_cast<const uint8_t*>([src_inst.buffer contents]) + desc.instance_offset);
      auto* dst_ptr = static_cast<MTLAccelerationStructureUserIDInstanceDescriptor*>(
          [mtl_inst_buf contents]);

      std::unordered_map<uint64_t, uint32_t> blas_index_map;
      blas_index_map.reserve(desc.instance_count);

      for (uint32_t i = 0; i < desc.instance_count; ++i) {
        const TlasInstanceDesc& si = src_ptr[i];
        id<MTLAccelerationStructure> blas = lookup_as_by_address(si.blas_device_address);
        if (!blas) continue;

        uint32_t blas_idx = 0;
        auto it = blas_index_map.find(si.blas_device_address);
        if (it != blas_index_map.end()) {
          blas_idx = it->second;
        } else {
          blas_idx = static_cast<uint32_t>([blas_list count]);
          [blas_list addObject:blas];
          blas_index_map[si.blas_device_address] = blas_idx;
        }

        MTLAccelerationStructureUserIDInstanceDescriptor di{};
        // Transpose row-major 3x4 TlasInstanceDesc transform into Metal column-major 4x3 matrix.
        for (int col = 0; col < 4; ++col) {
          di.transformationMatrix.columns[col][0] = si.transform[0][col];
          di.transformationMatrix.columns[col][1] = si.transform[1][col];
          di.transformationMatrix.columns[col][2] = si.transform[2][col];
        }
        di.options = to_mtl_instance_options(si.flags_8);
        di.mask = si.mask_8;
        di.intersectionFunctionTableOffset = si.sbt_record_offset_24;
        di.accelerationStructureIndex = blas_idx;
        di.userID = si.custom_index_24;
        dst_ptr[valid_instances++] = di;
      }
    }

    MTLInstanceAccelerationStructureDescriptor* tlas_desc =
        [MTLInstanceAccelerationStructureDescriptor descriptor];
    tlas_desc.instancedAccelerationStructures = blas_list;
    tlas_desc.instanceDescriptorBuffer = mtl_inst_buf;
    tlas_desc.instanceDescriptorBufferOffset = 0;
    tlas_desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    tlas_desc.instanceDescriptorStride = sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
    tlas_desc.instanceCount = valid_instances;
    tlas_desc.usage = to_mtl_as_usage(desc.flags);

    cmd.keep_transient_object(blas_list);
    set_tlas_instanced_blases(desc.dst, blas_list);

    id<MTLAccelerationStructureCommandEncoder> enc = cmd.ensure_accel_encoder();
    if (!enc) return;
    [enc buildAccelerationStructure:dst_as.handle
                         descriptor:tlas_desc
                      scratchBuffer:scratch.buffer
                scratchBufferOffset:desc.scratch_offset];
  }
}

}  // namespace shift::rhi::mtl
