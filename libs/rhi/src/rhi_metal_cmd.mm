#include "rhi_metal_internal.h"

#import <CoreGraphics/CoreGraphics.h>
#import <TargetConditionals.h>

#ifdef SHIFT_RHI_HAS_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#endif

#include <algorithm>
#include <cstring>

namespace shift::rhi::mtl {

// -----------------------------------------------------------------------------
// MetalCommandListImpl
// -----------------------------------------------------------------------------
MetalCommandListImpl::MetalCommandListImpl(MetalDeviceImpl& device, id<MTLCommandBuffer> cmd_buf,
                                           QueueKind queue_kind, bool reusable)
    : device_(device), queue_kind_(queue_kind), reusable_(reusable), cmd_buf_(cmd_buf) {
  if (device_.mtl_device()) {
    barrier_fence_ = [device_.mtl_device() newFence];
  }
}

MetalCommandListImpl::~MetalCommandListImpl() {
  end_active_encoder();
}

void MetalCommandListImpl::reset_command_buffer(id<MTLCommandBuffer> cmd_buf) {
  end_active_encoder();
  cmd_buf_ = cmd_buf;
  current_pipeline_ = {};
  current_pipeline_is_compute_ = false;
  current_primitive_type_ = MTLPrimitiveTypeTriangle;
  current_compute_tg_ = MTLSizeMake(1, 1, 1);
  bound_index_buffer_ = nil;
  bound_index_offset_ = 0;
  bound_index_type_ = MTLIndexTypeUInt32;
  push_const_size_ = 0;
  push_const_compute_dirty_ = false;
  compute_pipeline_dirty_ = false;
  for (uint32_t i = 0; i < kMaxBindSets; ++i) pending_compute_groups_[i] = {};
  rp_width_ = 0;
  rp_height_ = 0;
  fence_updated_ = false;
  fence_wait_pending_ = false;
  transient_buffers_.clear();
  transient_objects_.clear();
}

void MetalCommandListImpl::begin() {
  if (reusable_) {
    id<MTLCommandQueue> q = device_.mtl_queue(queue_kind_);
    id<MTLCommandBuffer> fresh = q ? [q commandBuffer] : nil;
    reset_command_buffer(fresh);
    return;
  }
  end_active_encoder();
  current_pipeline_ = {};
  current_pipeline_is_compute_ = false;
  push_const_size_ = 0;
  push_const_compute_dirty_ = false;
  compute_pipeline_dirty_ = false;
  for (uint32_t i = 0; i < kMaxBindSets; ++i) pending_compute_groups_[i] = {};
  fence_updated_ = false;
  fence_wait_pending_ = false;
}

void MetalCommandListImpl::end() {
  end_active_encoder();
}

void MetalCommandListImpl::end_active_encoder() {
  switch (active_encoder_) {
    case EncoderKind::Render:
      if (render_enc_) {
        if (barrier_fence_) {
          [render_enc_ updateFence:barrier_fence_ afterStages:MTLRenderStageFragment];
          fence_updated_ = true;
        }
        [render_enc_ endEncoding];
      }
      render_enc_ = nil;
      break;
    case EncoderKind::Compute:
      if (compute_enc_) {
        if (barrier_fence_) {
          [compute_enc_ updateFence:barrier_fence_];
          fence_updated_ = true;
        }
        [compute_enc_ endEncoding];
      }
      compute_enc_ = nil;
      break;
    case EncoderKind::Blit:
      if (blit_enc_) {
        if (barrier_fence_) {
          [blit_enc_ updateFence:barrier_fence_];
          fence_updated_ = true;
        }
        [blit_enc_ endEncoding];
      }
      blit_enc_ = nil;
      break;
    case EncoderKind::AccelStruct:
      if (accel_enc_) {
        if (barrier_fence_) {
          [accel_enc_ updateFence:barrier_fence_];
          fence_updated_ = true;
        }
        [accel_enc_ endEncoding];
      }
      accel_enc_ = nil;
      break;
    case EncoderKind::None:
      break;
  }
  active_encoder_ = EncoderKind::None;
}

id<MTLComputeCommandEncoder> MetalCommandListImpl::ensure_compute_encoder() {
  if (active_encoder_ == EncoderKind::Compute && compute_enc_) return compute_enc_;
  if (active_encoder_ == EncoderKind::Render) return nil;
  end_active_encoder();
  if (!cmd_buf_) return nil;
  compute_enc_ = [cmd_buf_ computeCommandEncoder];
  if (compute_enc_) {
    active_encoder_ = EncoderKind::Compute;
    if (fence_wait_pending_ && barrier_fence_) {
      [compute_enc_ waitForFence:barrier_fence_];
      fence_wait_pending_ = false;
    }
  }
  return compute_enc_;
}

id<MTLBlitCommandEncoder> MetalCommandListImpl::ensure_blit_encoder() {
  if (active_encoder_ == EncoderKind::Blit && blit_enc_) return blit_enc_;
  if (active_encoder_ == EncoderKind::Render) return nil;
  end_active_encoder();
  if (!cmd_buf_) return nil;
  blit_enc_ = [cmd_buf_ blitCommandEncoder];
  if (blit_enc_) {
    active_encoder_ = EncoderKind::Blit;
    if (fence_wait_pending_ && barrier_fence_) {
      [blit_enc_ waitForFence:barrier_fence_];
      fence_wait_pending_ = false;
    }
  }
  return blit_enc_;
}

id<MTLAccelerationStructureCommandEncoder> MetalCommandListImpl::ensure_accel_encoder() {
  if (active_encoder_ == EncoderKind::AccelStruct && accel_enc_) return accel_enc_;
  if (active_encoder_ == EncoderKind::Render) return nil;
  end_active_encoder();
  if (!cmd_buf_) return nil;
  accel_enc_ = [cmd_buf_ accelerationStructureCommandEncoder];
  if (accel_enc_) {
    active_encoder_ = EncoderKind::AccelStruct;
    if (fence_wait_pending_ && barrier_fence_) {
      [accel_enc_ waitForFence:barrier_fence_];
      fence_wait_pending_ = false;
    }
  }
  return accel_enc_;
}

void MetalCommandListImpl::barrier(const BarrierDesc& desc) {
  if (active_encoder_ == EncoderKind::Compute && compute_enc_) {
    if (desc.global_compute_uav || !desc.buffers.empty() || !desc.textures.empty()) {
      [compute_enc_ memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    }
    return;
  }
  if (active_encoder_ == EncoderKind::Render && render_enc_) {
    if (!desc.buffers.empty() || !desc.textures.empty()) {
      [render_enc_ memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures
                               afterStages:MTLRenderStageFragment
                              beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    }
    return;
  }
  // Across encoders (Blit <-> Compute <-> Render <-> AccelStruct), end the active encoder
  // (which updates barrier_fence_) and arm a wait on the next encoder so resources referenced
  // indirectly via bindless argument buffers / MTLResidencySet observe prior writes deterministically.
  if (desc.global_compute_uav || desc.global_as_build_to_read || desc.global_host_read ||
      !desc.buffers.empty() || !desc.textures.empty()) {
    end_active_encoder();
    if (fence_updated_) {
      fence_wait_pending_ = true;
    }
  }
}

void MetalCommandListImpl::reset_query_pool(QueryPoolHandle pool, uint32_t first, uint32_t count) {
  QueryPoolEntry qe{};
  if (!device_.lookup_query_pool_copy(pool, qe) || count == 0 || first + count > qe.count) return;
  if (qe.type == QueryType::AccelStructCompactedSize && qe.result_buffer) {
    auto* ptr = static_cast<uint64_t*>([qe.result_buffer contents]);
    std::memset(ptr + first, 0, count * sizeof(uint64_t));
  }
}

void MetalCommandListImpl::write_timestamp(QueryPoolHandle pool, uint32_t index, PipelineStage stage) {
  QueryPoolEntry qe{};
  if (!device_.lookup_query_pool_copy(pool, qe) || qe.type != QueryType::Timestamp) return;
  if (!qe.counter_buffer || index >= qe.count || !cmd_buf_) return;

  if (device_.supports_blit_boundary_counters() && active_encoder_ != EncoderKind::Render) {
    id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
    if (enc) {
      [enc sampleCountersInBuffer:qe.counter_buffer atSampleIndex:index withBarrier:YES];
    }
    return;
  }

  if (device_.supports_stage_boundary_counters() && active_encoder_ != EncoderKind::Render) {
    // Apple Silicon TBDR GPUs support AtStageBoundary counter sampling.
    // Encode a minimal 4-byte fill inside a dedicated blit pass so the driver never elides
    // the empty encoder and records an accurate hardware GPU timestamp at this command point.
    end_active_encoder();
    @autoreleasepool {
      id<MTLBuffer> stamp_scratch =
          [device_.mtl_device() newBufferWithLength:4 options:MTLResourceStorageModePrivate];
      if (!stamp_scratch) return;
      keep_transient_buffer(stamp_scratch);

      MTLBlitPassDescriptor* bpd = [MTLBlitPassDescriptor blitPassDescriptor];
      bpd.sampleBufferAttachments[0].sampleBuffer = qe.counter_buffer;
      if (has_flag(stage, PipelineStage::TopOfPipe)) {
        bpd.sampleBufferAttachments[0].startOfEncoderSampleIndex = index;
        bpd.sampleBufferAttachments[0].endOfEncoderSampleIndex = MTLCounterDontSample;
      } else {
        bpd.sampleBufferAttachments[0].startOfEncoderSampleIndex = MTLCounterDontSample;
        bpd.sampleBufferAttachments[0].endOfEncoderSampleIndex = index;
      }
      id<MTLBlitCommandEncoder> enc = [cmd_buf_ blitCommandEncoderWithDescriptor:bpd];
      if (enc) {
        [enc fillBuffer:stamp_scratch range:NSMakeRange(0, 4) value:0];
        [enc endEncoding];
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Dynamic Render Pass (Reversed-Z + MSAA color/depth resolve)
// -----------------------------------------------------------------------------
void MetalCommandListImpl::begin_render_pass(const RenderPassDesc& desc) {
  end_active_encoder();
  if (!cmd_buf_) return;

  @autoreleasepool {
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    for (size_t i = 0; i < desc.color_attachments.size(); ++i) {
      const ColorAttachmentDesc& ca = desc.color_attachments[i];
      TextureViewEntry tv{};
      if (!device_.lookup_texture_view_copy(ca.view, tv) || !tv.view) continue;
      MTLRenderPassColorAttachmentDescriptor* att = rpd.colorAttachments[i];
      att.texture = tv.view;
      att.loadAction = to_mtl_load_action(ca.load_op);
      att.clearColor = MTLClearColorMake(ca.clear_color[0], ca.clear_color[1],
                                         ca.clear_color[2], ca.clear_color[3]);
      bool has_resolve = false;
      if (ca.resolve_view && ca.resolve_mode != ResolveMode::None) {
        TextureViewEntry rtv{};
        if (device_.lookup_texture_view_copy(ca.resolve_view, rtv) && rtv.view) {
          att.resolveTexture = rtv.view;
          has_resolve = true;
        }
      }
      att.storeAction = to_mtl_store_action(ca.store_op, has_resolve);
    }

    if (desc.depth_attachment && desc.depth_attachment->view) {
      const DepthAttachmentDesc& da = *desc.depth_attachment;
      TextureViewEntry dtv{};
      if (device_.lookup_texture_view_copy(da.view, dtv) && dtv.view) {
        MTLRenderPassDepthAttachmentDescriptor* datt = rpd.depthAttachment;
        datt.texture = dtv.view;
        datt.loadAction = to_mtl_load_action(da.load_op);
        datt.clearDepth = static_cast<double>(da.clear_depth);
        bool has_resolve = false;
        if (da.resolve_view && da.resolve_mode != ResolveMode::None) {
          TextureViewEntry rdv{};
          if (device_.lookup_texture_view_copy(da.resolve_view, rdv) && rdv.view) {
            datt.resolveTexture = rdv.view;
            datt.depthResolveFilter = to_mtl_depth_resolve_filter(da.resolve_mode);
            has_resolve = true;
          }
        }
        datt.storeAction = to_mtl_store_action(da.store_op, has_resolve);
      }
    }

    render_enc_ = [cmd_buf_ renderCommandEncoderWithDescriptor:rpd];
    if (render_enc_) {
      active_encoder_ = EncoderKind::Render;
      rp_width_ = desc.width;
      rp_height_ = desc.height;
      if (fence_wait_pending_ && barrier_fence_) {
        [render_enc_ waitForFence:barrier_fence_
                     beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
        fence_wait_pending_ = false;
      }
      if (desc.debug_label && *desc.debug_label) {
        render_enc_.label = [NSString stringWithUTF8String:desc.debug_label];
      }
    }
  }
}

void MetalCommandListImpl::end_render_pass() {
  if (active_encoder_ == EncoderKind::Render) {
    end_active_encoder();
  }
}

// -----------------------------------------------------------------------------
// Dynamic raster state
// -----------------------------------------------------------------------------
void MetalCommandListImpl::set_viewport(float x, float y, float width, float height,
                                        float min_depth, float max_depth) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_) return;
  MTLViewport vp{};
  vp.originX = static_cast<double>(x);
  vp.originY = static_cast<double>(y);
  vp.width = static_cast<double>(width);
  vp.height = static_cast<double>(height);
  vp.znear = static_cast<double>(min_depth);
  vp.zfar = static_cast<double>(max_depth);
  [render_enc_ setViewport:vp];
}

void MetalCommandListImpl::set_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_) return;
  const uint32_t sx = static_cast<uint32_t>(std::max(x, 0));
  const uint32_t sy = static_cast<uint32_t>(std::max(y, 0));
  uint32_t sw = width;
  uint32_t sh = height;
  if (rp_width_ > 0 && sx < rp_width_) sw = std::min(sw, rp_width_ - sx);
  if (rp_height_ > 0 && sy < rp_height_) sh = std::min(sh, rp_height_ - sy);
  MTLScissorRect sc{sx, sy, sw, sh};
  [render_enc_ setScissorRect:sc];
}

void MetalCommandListImpl::set_depth_bias(float constant_factor, float clamp, float slope_factor) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_) return;
  [render_enc_ setDepthBias:constant_factor slopeScale:slope_factor clamp:clamp];
}

void MetalCommandListImpl::set_blend_constants(const float rgba[4]) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_ || !rgba) return;
  [render_enc_ setBlendColorRed:rgba[0] green:rgba[1] blue:rgba[2] alpha:rgba[3]];
}

void MetalCommandListImpl::set_shading_rate(uint32_t /*width*/, uint32_t /*height*/) {
  // Variable rate shading on Metal uses MTLRasterizationRateMap attached to the render pass.
}

// -----------------------------------------------------------------------------
// Pipelines, bind groups, bindless, push constants, vertex/index buffers
// -----------------------------------------------------------------------------
void MetalCommandListImpl::apply_render_pipeline_state(const PipelineEntry& pe) {
  if (!render_enc_ || !pe.render_pso) return;
  [render_enc_ setRenderPipelineState:pe.render_pso];
  if (pe.depth_stencil) [render_enc_ setDepthStencilState:pe.depth_stencil];
  [render_enc_ setCullMode:pe.cull_mode];
  [render_enc_ setFrontFacingWinding:pe.front_face];
  [render_enc_ setDepthClipMode:pe.depth_clip_mode];
  current_primitive_type_ = pe.primitive_type;
}

void MetalCommandListImpl::bind_pipeline(PipelineHandle pipeline) {
  PipelineEntry pe{};
  if (!device_.lookup_pipeline_copy(pipeline, pe)) return;
  current_pipeline_ = pipeline;
  current_pipeline_is_compute_ = pe.is_compute;

  if (pe.is_compute) {
    current_compute_tg_ = pe.compute_threads_per_group;
    compute_pipeline_dirty_ = true;
    if (active_encoder_ == EncoderKind::Compute && compute_enc_ && pe.compute_pso) {
      [compute_enc_ setComputePipelineState:pe.compute_pso];
      compute_pipeline_dirty_ = false;
    }
  } else {
    if (active_encoder_ == EncoderKind::Render && render_enc_) {
      apply_render_pipeline_state(pe);
    }
  }
}

void MetalCommandListImpl::apply_bind_group_to_render(uint32_t set_index, const BindGroupEntry& ge) {
  if (!render_enc_) return;
  if (ge.bindless_table) {
    ge.bindless_table->commit_residency_if_needed();
    id<MTLBuffer> arg_buf = ge.bindless_table->arg_buffer();
    if (arg_buf) {
      [render_enc_ setVertexBuffer:arg_buf offset:0 atIndex:set_index];
      [render_enc_ setFragmentBuffer:arg_buf offset:0 atIndex:set_index];
      ge.bindless_table->encode_use_resources_render(render_enc_);
    }
    return;
  }

  const uint32_t base_slot = (set_index == 0) ? 0u : (set_index * 8u);
  for (const ResolvedBindEntry& re : ge.entries) {
    const NSUInteger idx = base_slot + re.binding + re.array_element;
    if (re.type == DescriptorType::UniformBuffer || re.type == DescriptorType::StorageBuffer) {
      if (re.buffer && idx < kVertexBufferBaseIndex) {
        [render_enc_ setVertexBuffer:re.buffer offset:re.buffer_offset atIndex:idx];
        [render_enc_ setFragmentBuffer:re.buffer offset:re.buffer_offset atIndex:idx];
      }
    } else if (re.type == DescriptorType::SampledTexture ||
               re.type == DescriptorType::StorageTexture) {
      if (re.texture) {
        [render_enc_ setVertexTexture:re.texture atIndex:idx];
        [render_enc_ setFragmentTexture:re.texture atIndex:idx];
      }
    } else if (re.type == DescriptorType::Sampler) {
      if (re.sampler) {
        [render_enc_ setVertexSamplerState:re.sampler atIndex:idx];
        [render_enc_ setFragmentSamplerState:re.sampler atIndex:idx];
      }
    } else if (re.type == DescriptorType::CombinedTextureSampler) {
      if (re.texture) {
        [render_enc_ setVertexTexture:re.texture atIndex:idx];
        [render_enc_ setFragmentTexture:re.texture atIndex:idx];
      }
      if (re.sampler) {
        [render_enc_ setVertexSamplerState:re.sampler atIndex:idx];
        [render_enc_ setFragmentSamplerState:re.sampler atIndex:idx];
      }
    } else if (re.type == DescriptorType::AccelStruct) {
      if (re.accel_struct && idx < kVertexBufferBaseIndex) {
        [render_enc_ setFragmentAccelerationStructure:re.accel_struct atBufferIndex:idx];
        [render_enc_ useResource:re.accel_struct
                           usage:MTLResourceUsageRead
                          stages:MTLRenderStageFragment];
        for (id<MTLAccelerationStructure> b : re.tlas_blases) {
          if (b) {
            [render_enc_ useResource:b usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
          }
        }
      }
    }
  }
}

void MetalCommandListImpl::apply_bind_group_to_compute(uint32_t set_index, const BindGroupEntry& ge) {
  if (!compute_enc_) return;
  if (ge.bindless_table) {
    ge.bindless_table->commit_residency_if_needed();
    id<MTLBuffer> arg_buf = ge.bindless_table->arg_buffer();
    if (arg_buf) {
      [compute_enc_ setBuffer:arg_buf offset:0 atIndex:set_index];
      ge.bindless_table->encode_use_resources_compute(compute_enc_);
    }
    return;
  }

  const uint32_t base_slot = (set_index == 0) ? 0u : (set_index * 8u);
  for (const ResolvedBindEntry& re : ge.entries) {
    const NSUInteger idx = base_slot + re.binding + re.array_element;
    if (re.type == DescriptorType::UniformBuffer || re.type == DescriptorType::StorageBuffer) {
      if (re.buffer && idx < kPushConstantBufferIndex) {
        [compute_enc_ setBuffer:re.buffer offset:re.buffer_offset atIndex:idx];
      }
    } else if (re.type == DescriptorType::SampledTexture ||
               re.type == DescriptorType::StorageTexture) {
      if (re.texture) {
        [compute_enc_ setTexture:re.texture atIndex:idx];
      }
    } else if (re.type == DescriptorType::Sampler) {
      if (re.sampler) {
        [compute_enc_ setSamplerState:re.sampler atIndex:idx];
      }
    } else if (re.type == DescriptorType::CombinedTextureSampler) {
      if (re.texture) [compute_enc_ setTexture:re.texture atIndex:idx];
      if (re.sampler) [compute_enc_ setSamplerState:re.sampler atIndex:idx];
    } else if (re.type == DescriptorType::AccelStruct) {
      if (re.accel_struct && idx < kPushConstantBufferIndex) {
        [compute_enc_ setAccelerationStructure:re.accel_struct atBufferIndex:idx];
        [compute_enc_ useResource:re.accel_struct usage:MTLResourceUsageRead];
        for (id<MTLAccelerationStructure> b : re.tlas_blases) {
          if (b) [compute_enc_ useResource:b usage:MTLResourceUsageRead];
        }
      }
    }
  }
}

void MetalCommandListImpl::bind_group(uint32_t set_index, BindGroupHandle group) {
  BindGroupEntry ge{};
  if (!device_.lookup_bind_group_copy(group, ge)) return;

  if (active_encoder_ == EncoderKind::Render && render_enc_) {
    apply_bind_group_to_render(set_index, ge);
    return;
  }

  if (set_index < kMaxBindSets) {
    pending_compute_groups_[set_index] = group;
  }
  if (active_encoder_ == EncoderKind::Compute && compute_enc_) {
    apply_bind_group_to_compute(set_index, ge);
  }
}

void MetalCommandListImpl::bind_bindless(uint32_t set_index, const BindlessTable& table) {
  bind_group(set_index, table.bind_group());
}

void MetalCommandListImpl::push_constants(ShaderStage stages, uint32_t offset, uint32_t size,
                                          const void* data) {
  if (!data || size == 0 || offset + size > kMaxPushConstantBytes) return;
  std::memcpy(push_const_data_ + offset, data, size);
  push_const_size_ = std::max(push_const_size_, offset + size);

  if (active_encoder_ == EncoderKind::Render && render_enc_) {
    if (has_flag(stages, ShaderStage::Vertex)) {
      [render_enc_ setVertexBytes:push_const_data_
                           length:push_const_size_
                          atIndex:kPushConstantBufferIndex];
    }
    if (has_flag(stages, ShaderStage::Fragment)) {
      [render_enc_ setFragmentBytes:push_const_data_
                             length:push_const_size_
                            atIndex:kPushConstantBufferIndex];
    }
  }
  if (has_flag(stages, ShaderStage::Compute)) {
    if (active_encoder_ == EncoderKind::Compute && compute_enc_) {
      [compute_enc_ setBytes:push_const_data_
                      length:push_const_size_
                     atIndex:kPushConstantBufferIndex];
      push_const_compute_dirty_ = false;
    } else {
      push_const_compute_dirty_ = true;
    }
  }
}

void MetalCommandListImpl::bind_vertex_buffers(uint32_t first_binding,
                                               std::span<const BufferHandle> buffers,
                                               std::span<const uint64_t> offsets) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_) return;
  for (size_t i = 0; i < buffers.size(); ++i) {
    BufferEntry be{};
    if (!device_.lookup_buffer_copy(buffers[i], be) || !be.buffer) continue;
    const uint64_t off = (i < offsets.size()) ? offsets[i] : 0ull;
    const NSUInteger slot = kVertexBufferBaseIndex + first_binding + i;
    if (slot < kPushConstantBufferIndex) {
      [render_enc_ setVertexBuffer:be.buffer offset:off atIndex:slot];
    }
  }
}

void MetalCommandListImpl::bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type) {
  BufferEntry be{};
  if (!device_.lookup_buffer_copy(buffer, be) || !be.buffer) {
    bound_index_buffer_ = nil;
    return;
  }
  bound_index_buffer_ = be.buffer;
  bound_index_offset_ = offset;
  bound_index_type_ = (type == IndexType::Uint16) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

// -----------------------------------------------------------------------------
// Draws & Indirect Draws
// -----------------------------------------------------------------------------
void MetalCommandListImpl::draw(uint32_t vertex_count, uint32_t instance_count,
                                uint32_t first_vertex, uint32_t first_instance) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_) return;
  [render_enc_ drawPrimitives:current_primitive_type_
                  vertexStart:first_vertex
                  vertexCount:vertex_count
                instanceCount:instance_count
                 baseInstance:first_instance];
}

void MetalCommandListImpl::draw_indexed(uint32_t index_count, uint32_t instance_count,
                                        uint32_t first_index, int32_t vertex_offset,
                                        uint32_t first_instance) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_ || !bound_index_buffer_) return;
  const uint64_t elem_bytes = (bound_index_type_ == MTLIndexTypeUInt16) ? 2ull : 4ull;
  [render_enc_ drawIndexedPrimitives:current_primitive_type_
                          indexCount:index_count
                           indexType:bound_index_type_
                         indexBuffer:bound_index_buffer_
                   indexBufferOffset:bound_index_offset_ + static_cast<uint64_t>(first_index) * elem_bytes
                       instanceCount:instance_count
                          baseVertex:vertex_offset
                        baseInstance:first_instance];
}

void MetalCommandListImpl::draw_indirect(BufferHandle arg_buffer, uint64_t arg_offset,
                                         uint32_t draw_count, uint32_t stride) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_ || draw_count == 0) return;
  BufferEntry be{};
  if (!device_.lookup_buffer_copy(arg_buffer, be) || !be.buffer) return;
  const uint32_t step = (stride > 0) ? stride : sizeof(MTLDrawPrimitivesIndirectArguments);
  for (uint32_t i = 0; i < draw_count; ++i) {
    [render_enc_ drawPrimitives:current_primitive_type_
                 indirectBuffer:be.buffer
           indirectBufferOffset:arg_offset + static_cast<uint64_t>(i) * step];
  }
}

void MetalCommandListImpl::draw_indexed_indirect_count(BufferHandle arg_buffer, uint64_t arg_offset,
                                                       BufferHandle count_buffer, uint64_t count_offset,
                                                       uint32_t max_draw_count, uint32_t stride) {
  if (active_encoder_ != EncoderKind::Render || !render_enc_ || !bound_index_buffer_ ||
      max_draw_count == 0) {
    return;
  }
  BufferEntry ab{};
  BufferEntry cb{};
  if (!device_.lookup_buffer_copy(arg_buffer, ab) || !ab.buffer) return;
  uint32_t effective_count = max_draw_count;
  if (device_.lookup_buffer_copy(count_buffer, cb) && cb.buffer &&
      [cb.buffer storageMode] != MTLStorageModePrivate &&
      count_offset + sizeof(uint32_t) <= cb.size) {
    uint32_t cpu_count = 0;
    std::memcpy(&cpu_count, static_cast<const uint8_t*>([cb.buffer contents]) + count_offset,
                sizeof(uint32_t));
    effective_count = std::min(max_draw_count, cpu_count);
  }
  const uint32_t step = (stride > 0) ? stride : sizeof(MTLDrawIndexedPrimitivesIndirectArguments);
  for (uint32_t i = 0; i < effective_count; ++i) {
    [render_enc_ drawIndexedPrimitives:current_primitive_type_
                             indexType:bound_index_type_
                           indexBuffer:bound_index_buffer_
                     indexBufferOffset:bound_index_offset_
                        indirectBuffer:ab.buffer
                  indirectBufferOffset:arg_offset + static_cast<uint64_t>(i) * step];
  }
}

// -----------------------------------------------------------------------------
// Compute Dispatch
// -----------------------------------------------------------------------------
void MetalCommandListImpl::flush_pending_compute_state() {
  if (!compute_enc_) return;
  if (compute_pipeline_dirty_ && current_pipeline_ && current_pipeline_is_compute_) {
    PipelineEntry pe{};
    if (device_.lookup_pipeline_copy(current_pipeline_, pe) && pe.compute_pso) {
      [compute_enc_ setComputePipelineState:pe.compute_pso];
      current_compute_tg_ = pe.compute_threads_per_group;
    }
    compute_pipeline_dirty_ = false;
  }
  for (uint32_t s = 0; s < kMaxBindSets; ++s) {
    if (pending_compute_groups_[s]) {
      BindGroupEntry ge{};
      if (device_.lookup_bind_group_copy(pending_compute_groups_[s], ge)) {
        apply_bind_group_to_compute(s, ge);
      }
    }
  }
  if (push_const_compute_dirty_ && push_const_size_ > 0) {
    [compute_enc_ setBytes:push_const_data_
                    length:push_const_size_
                   atIndex:kPushConstantBufferIndex];
    push_const_compute_dirty_ = false;
  }
}

void MetalCommandListImpl::dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z) {
  if (group_x == 0 || group_y == 0 || group_z == 0) return;
  const bool opening_new = (active_encoder_ != EncoderKind::Compute);
  id<MTLComputeCommandEncoder> enc = ensure_compute_encoder();
  if (!enc) return;
  if (opening_new) {
    compute_pipeline_dirty_ = true;
    push_const_compute_dirty_ = (push_const_size_ > 0);
  }
  flush_pending_compute_state();
  [enc dispatchThreadgroups:MTLSizeMake(group_x, group_y, group_z)
      threadsPerThreadgroup:current_compute_tg_];
}

void MetalCommandListImpl::dispatch_indirect(BufferHandle arg_buffer, uint64_t arg_offset) {
  BufferEntry ab{};
  if (!device_.lookup_buffer_copy(arg_buffer, ab) || !ab.buffer) return;
  const bool opening_new = (active_encoder_ != EncoderKind::Compute);
  id<MTLComputeCommandEncoder> enc = ensure_compute_encoder();
  if (!enc) return;
  if (opening_new) {
    compute_pipeline_dirty_ = true;
    push_const_compute_dirty_ = (push_const_size_ > 0);
  }
  flush_pending_compute_state();
  [enc dispatchThreadgroupsWithIndirectBuffer:ab.buffer
                         indirectBufferOffset:arg_offset
                        threadsPerThreadgroup:current_compute_tg_];
}

// -----------------------------------------------------------------------------
// Transfers, Clears, Blits, and Depth Resolve
// -----------------------------------------------------------------------------
void MetalCommandListImpl::update_buffer(BufferHandle dst, uint64_t offset, uint64_t size,
                                         const void* data) {
  if (!data || size == 0) return;
  BufferEntry db{};
  if (!device_.lookup_buffer_copy(dst, db) || !db.buffer) return;

  @autoreleasepool {
    id<MTLBuffer> tmp = [device_.mtl_device() newBufferWithBytes:data
                                                          length:size
                                                         options:MTLResourceStorageModeShared];
    if (!tmp) return;
    keep_transient_buffer(tmp);

    id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
    if (!enc) return;
    [enc copyFromBuffer:tmp sourceOffset:0 toBuffer:db.buffer destinationOffset:offset size:size];
  }
}

void MetalCommandListImpl::fill_buffer(BufferHandle dst, uint64_t offset, uint64_t size,
                                       uint32_t value) {
  BufferEntry db{};
  if (!device_.lookup_buffer_copy(dst, db) || !db.buffer) return;
  const uint64_t actual_size =
      (size == UINT64_MAX) ? (db.size > offset ? db.size - offset : 0ull) : size;
  if (actual_size == 0) return;

  id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
  if (!enc) return;

  // Fast path: all 4 bytes identical -> native MTLBlitCommandEncoder fillBuffer.
  const uint8_t b0 = static_cast<uint8_t>(value & 0xFFu);
  const uint8_t b1 = static_cast<uint8_t>((value >> 8) & 0xFFu);
  const uint8_t b2 = static_cast<uint8_t>((value >> 16) & 0xFFu);
  const uint8_t b3 = static_cast<uint8_t>((value >> 24) & 0xFFu);
  if (b0 == b1 && b1 == b2 && b2 == b3) {
    [enc fillBuffer:db.buffer range:NSMakeRange(offset, actual_size) value:b0];
    return;
  }

  // 32-bit pattern fill via transient shared staging buffer (chunked up to 1 MiB).
  @autoreleasepool {
    constexpr uint64_t kChunkBytes = 1024ull * 1024ull;
    const uint64_t stage_bytes = std::min(actual_size, kChunkBytes);
    id<MTLBuffer> stage = [device_.mtl_device() newBufferWithLength:stage_bytes
                                                            options:MTLResourceStorageModeShared];
    if (!stage) return;
    keep_transient_buffer(stage);
    auto* words = static_cast<uint32_t*>([stage contents]);
    const uint64_t nwords = stage_bytes / sizeof(uint32_t);
    for (uint64_t i = 0; i < nwords; ++i) words[i] = value;

    uint64_t remaining = actual_size;
    uint64_t dst_off = offset;
    while (remaining > 0) {
      const uint64_t n = std::min(remaining, stage_bytes);
      [enc copyFromBuffer:stage sourceOffset:0 toBuffer:db.buffer destinationOffset:dst_off size:n];
      dst_off += n;
      remaining -= n;
    }
  }
}

void MetalCommandListImpl::copy_buffer(BufferHandle src, uint64_t src_offset,
                                       BufferHandle dst, uint64_t dst_offset, uint64_t size) {
  if (size == 0) return;
  BufferEntry sb{}, db{};
  if (!device_.lookup_buffer_copy(src, sb) || !sb.buffer) return;
  if (!device_.lookup_buffer_copy(dst, db) || !db.buffer) return;
  id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
  if (!enc) return;
  [enc copyFromBuffer:sb.buffer
         sourceOffset:src_offset
             toBuffer:db.buffer
    destinationOffset:dst_offset
                 size:size];
}

void MetalCommandListImpl::copy_buffer_to_texture(BufferHandle src, TextureHandle dst,
                                                  std::span<const BufferTextureCopy> regions) {
  BufferEntry sb{};
  TextureEntry dt{};
  if (!device_.lookup_buffer_copy(src, sb) || !sb.buffer) return;
  if (!device_.lookup_texture_copy(dst, dt) || !dt.texture || regions.empty()) return;
  id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
  if (!enc) return;

  const uint32_t blk = format_block_extent(dt.format);
  const uint32_t bpb = format_bytes_per_block(dt.format);

  for (const BufferTextureCopy& r : regions) {
    const uint32_t row_texels = r.buffer_row_length > 0 ? r.buffer_row_length : r.width;
    const uint32_t img_rows = r.buffer_image_height > 0 ? r.buffer_image_height : r.height;
    const uint64_t row_blocks = (static_cast<uint64_t>(row_texels) + blk - 1ull) / blk;
    const uint64_t col_blocks = (static_cast<uint64_t>(img_rows) + blk - 1ull) / blk;
    const NSUInteger bytes_per_row = static_cast<NSUInteger>(row_blocks * bpb);
    const NSUInteger bytes_per_image = static_cast<NSUInteger>(col_blocks * bytes_per_row);

    for (uint32_t layer = 0; layer < r.layer_count; ++layer) {
      const NSUInteger layer_off = static_cast<NSUInteger>(r.buffer_offset + layer * bytes_per_image);
      [enc copyFromBuffer:sb.buffer
               sourceOffset:layer_off
          sourceBytesPerRow:bytes_per_row
        sourceBytesPerImage:bytes_per_image
                 sourceSize:MTLSizeMake(r.width, r.height, std::max(r.depth, 1u))
                  toTexture:dt.texture
           destinationSlice:r.base_layer + layer
           destinationLevel:r.mip_level
          destinationOrigin:MTLOriginMake(r.tex_x, r.tex_y, r.tex_z)];
    }
  }
}

void MetalCommandListImpl::copy_texture_to_buffer(TextureHandle src, BufferHandle dst,
                                                  std::span<const BufferTextureCopy> regions) {
  TextureEntry st{};
  BufferEntry db{};
  if (!device_.lookup_texture_copy(src, st) || !st.texture) return;
  if (!device_.lookup_buffer_copy(dst, db) || !db.buffer || regions.empty()) return;
  id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
  if (!enc) return;

  const uint32_t blk = format_block_extent(st.format);
  const uint32_t bpb = format_bytes_per_block(st.format);

  for (const BufferTextureCopy& r : regions) {
    const uint32_t row_texels = r.buffer_row_length > 0 ? r.buffer_row_length : r.width;
    const uint32_t img_rows = r.buffer_image_height > 0 ? r.buffer_image_height : r.height;
    const uint64_t row_blocks = (static_cast<uint64_t>(row_texels) + blk - 1ull) / blk;
    const uint64_t col_blocks = (static_cast<uint64_t>(img_rows) + blk - 1ull) / blk;
    const NSUInteger bytes_per_row = static_cast<NSUInteger>(row_blocks * bpb);
    const NSUInteger bytes_per_image = static_cast<NSUInteger>(col_blocks * bytes_per_row);

    for (uint32_t layer = 0; layer < r.layer_count; ++layer) {
      const NSUInteger layer_off = static_cast<NSUInteger>(r.buffer_offset + layer * bytes_per_image);
      [enc copyFromTexture:st.texture
               sourceSlice:r.base_layer + layer
               sourceLevel:r.mip_level
              sourceOrigin:MTLOriginMake(r.tex_x, r.tex_y, r.tex_z)
                sourceSize:MTLSizeMake(r.width, r.height, std::max(r.depth, 1u))
                  toBuffer:db.buffer
         destinationOffset:layer_off
    destinationBytesPerRow:bytes_per_row
  destinationBytesPerImage:bytes_per_image];
    }
  }
}

void MetalCommandListImpl::copy_texture(TextureHandle src, TextureHandle dst,
                                        const TextureCopyRegion& r) {
  TextureEntry st{}, dt{};
  if (!device_.lookup_texture_copy(src, st) || !st.texture) return;
  if (!device_.lookup_texture_copy(dst, dt) || !dt.texture) return;
  id<MTLBlitCommandEncoder> enc = ensure_blit_encoder();
  if (!enc) return;

  for (uint32_t layer = 0; layer < r.layer_count; ++layer) {
    [enc copyFromTexture:st.texture
             sourceSlice:r.src_layer + layer
             sourceLevel:r.src_mip
            sourceOrigin:MTLOriginMake(r.src_x, r.src_y, 0)
              sourceSize:MTLSizeMake(r.width, r.height, 1)
               toTexture:dt.texture
        destinationSlice:r.dst_layer + layer
        destinationLevel:r.dst_mip
       destinationOrigin:MTLOriginMake(r.dst_x, r.dst_y, 0)];
  }
}

void MetalCommandListImpl::blit(TextureHandle src, TextureHandle dst,
                                const TextureBlitRegion& r, Filter filter) {
  TextureEntry st{}, dt{};
  if (!device_.lookup_texture_copy(src, st) || !st.texture) return;
  if (!device_.lookup_texture_copy(dst, dt) || !dt.texture) return;

  const int32_t sw = r.src_x1 - r.src_x0;
  const int32_t sh = r.src_y1 - r.src_y0;
  const int32_t dw = r.dst_x1 - r.dst_x0;
  const int32_t dh = r.dst_y1 - r.dst_y0;

  // Fast 1:1 direct blit copy when dimensions & pixel format match without flip.
  if (sw == dw && sh == dh && sw > 0 && sh > 0 && st.mtl_format == dt.mtl_format) {
    TextureCopyRegion cr{};
    cr.src_mip = r.src_mip;
    cr.src_layer = r.src_layer;
    cr.src_x = r.src_x0;
    cr.src_y = r.src_y0;
    cr.dst_mip = r.dst_mip;
    cr.dst_layer = r.dst_layer;
    cr.dst_x = r.dst_x0;
    cr.dst_y = r.dst_y0;
    cr.width = static_cast<uint32_t>(sw);
    cr.height = static_cast<uint32_t>(sh);
    cr.layer_count = r.layer_count;
    cr.is_depth = r.is_depth;
    copy_texture(src, dst, cr);
    return;
  }

  // Scaled/filtered blit via internal fullscreen-triangle Metal render pass.
  end_active_encoder();
  if (!cmd_buf_) return;

  id<MTLRenderPipelineState> pso = device_.get_or_create_blit_pipeline(dt.mtl_format);
  id<MTLSamplerState> samp = device_.get_or_create_blit_sampler(filter);
  if (!pso || !samp) return;

  @autoreleasepool {
    const uint32_t src_mip_w = std::max(1u, st.width >> r.src_mip);
    const uint32_t src_mip_h = std::max(1u, st.height >> r.src_mip);
    const float uv_rect[4] = {
        static_cast<float>(r.src_x0) / static_cast<float>(src_mip_w),
        static_cast<float>(r.src_y0) / static_cast<float>(src_mip_h),
        static_cast<float>(r.src_x1) / static_cast<float>(src_mip_w),
        static_cast<float>(r.src_y1) / static_cast<float>(src_mip_h),
    };

    const int32_t dx0 = std::min(r.dst_x0, r.dst_x1);
    const int32_t dy0 = std::min(r.dst_y0, r.dst_y1);
    const uint32_t d_w = static_cast<uint32_t>(std::abs(dw));
    const uint32_t d_h = static_cast<uint32_t>(std::abs(dh));
    if (d_w == 0 || d_h == 0) return;

    for (uint32_t layer = 0; layer < r.layer_count; ++layer) {
      MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
      rpd.colorAttachments[0].texture = dt.texture;
      rpd.colorAttachments[0].level = r.dst_mip;
      rpd.colorAttachments[0].slice = r.dst_layer + layer;
      rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

      id<MTLRenderCommandEncoder> enc = [cmd_buf_ renderCommandEncoderWithDescriptor:rpd];
      if (!enc) continue;
      if (fence_wait_pending_ && barrier_fence_) {
        [enc waitForFence:barrier_fence_ beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
        fence_wait_pending_ = false;
      }
      MTLViewport vp{static_cast<double>(dx0), static_cast<double>(dy0),
                     static_cast<double>(d_w), static_cast<double>(d_h), 0.0, 1.0};
      [enc setViewport:vp];
      [enc setRenderPipelineState:pso];
      [enc setVertexBytes:uv_rect length:sizeof(uv_rect) atIndex:0];
      id<MTLTexture> src_view = st.texture;
      if (r.src_mip > 0 || r.src_layer + layer > 0 || st.array_layers > 1) {
        src_view = [st.texture newTextureViewWithPixelFormat:st.mtl_format
                                                 textureType:MTLTextureType2D
                                                      levels:NSMakeRange(r.src_mip, 1)
                                                      slices:NSMakeRange(r.src_layer + layer, 1)];
      }
      [enc setFragmentTexture:src_view atIndex:0];
      [enc setFragmentSamplerState:samp atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      if (barrier_fence_) {
        [enc updateFence:barrier_fence_ afterStages:MTLRenderStageFragment];
        fence_updated_ = true;
      }
      [enc endEncoding];
    }
  }
}

void MetalCommandListImpl::clear_color_texture(TextureHandle dst, const float rgba[4]) {
  if (!rgba || !cmd_buf_) return;
  TextureEntry dt{};
  if (!device_.lookup_texture_copy(dst, dt) || !dt.texture) return;
  end_active_encoder();

  @autoreleasepool {
    for (uint32_t mip = 0; mip < dt.mip_levels; ++mip) {
      for (uint32_t layer = 0; layer < dt.array_layers; ++layer) {
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = dt.texture;
        rpd.colorAttachments[0].level = mip;
        rpd.colorAttachments[0].slice = layer;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor =
            MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
        id<MTLRenderCommandEncoder> enc = [cmd_buf_ renderCommandEncoderWithDescriptor:rpd];
        if (enc) {
          if (fence_wait_pending_ && barrier_fence_) {
            [enc waitForFence:barrier_fence_ beforeStages:MTLRenderStageFragment];
            fence_wait_pending_ = false;
          }
          if (barrier_fence_) {
            [enc updateFence:barrier_fence_ afterStages:MTLRenderStageFragment];
            fence_updated_ = true;
          }
          [enc endEncoding];
        }
      }
    }
  }
}

void MetalCommandListImpl::clear_depth_texture(TextureHandle dst, float depth) {
  if (!cmd_buf_) return;
  TextureEntry dt{};
  if (!device_.lookup_texture_copy(dst, dt) || !dt.texture) return;
  end_active_encoder();

  @autoreleasepool {
    for (uint32_t mip = 0; mip < dt.mip_levels; ++mip) {
      for (uint32_t layer = 0; layer < dt.array_layers; ++layer) {
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.depthAttachment.texture = dt.texture;
        rpd.depthAttachment.level = mip;
        rpd.depthAttachment.slice = layer;
        rpd.depthAttachment.loadAction = MTLLoadActionClear;
        rpd.depthAttachment.storeAction = MTLStoreActionStore;
        rpd.depthAttachment.clearDepth = static_cast<double>(depth);
        id<MTLRenderCommandEncoder> enc = [cmd_buf_ renderCommandEncoderWithDescriptor:rpd];
        if (enc) {
          if (fence_wait_pending_ && barrier_fence_) {
            [enc waitForFence:barrier_fence_ beforeStages:MTLRenderStageFragment];
            fence_wait_pending_ = false;
          }
          if (barrier_fence_) {
            [enc updateFence:barrier_fence_ afterStages:MTLRenderStageFragment];
            fence_updated_ = true;
          }
          [enc endEncoding];
        }
      }
    }
  }
}

void MetalCommandListImpl::resolve_depth(TextureViewHandle msaa_depth,
                                         TextureViewHandle resolved_depth,
                                         uint32_t /*width*/, uint32_t /*height*/,
                                         ResolveMode mode) {
  if (!cmd_buf_) return;
  TextureViewEntry src{}, dst{};
  if (!device_.lookup_texture_view_copy(msaa_depth, src) || !src.view) return;
  if (!device_.lookup_texture_view_copy(resolved_depth, dst) || !dst.view) return;
  end_active_encoder();

  @autoreleasepool {
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.depthAttachment.texture = src.view;
    rpd.depthAttachment.resolveTexture = dst.view;
    rpd.depthAttachment.loadAction = MTLLoadActionLoad;
    rpd.depthAttachment.storeAction =
        ([src.view storageMode] == MTLStorageModeMemoryless)
            ? MTLStoreActionMultisampleResolve
            : MTLStoreActionStoreAndMultisampleResolve;
    rpd.depthAttachment.depthResolveFilter =
        to_mtl_depth_resolve_filter(mode == ResolveMode::None ? ResolveMode::SampleZero : mode);
    id<MTLRenderCommandEncoder> enc = [cmd_buf_ renderCommandEncoderWithDescriptor:rpd];
    if (enc) {
      if (fence_wait_pending_ && barrier_fence_) {
        [enc waitForFence:barrier_fence_ beforeStages:MTLRenderStageFragment];
        fence_wait_pending_ = false;
      }
      if (barrier_fence_) {
        [enc updateFence:barrier_fence_ afterStages:MTLRenderStageFragment];
        fence_updated_ = true;
      }
      [enc endEncoding];
    }
  }
}

// -----------------------------------------------------------------------------
// Ray Tracing Commands (delegated to rhi_metal_rt.mm)
// -----------------------------------------------------------------------------
void MetalCommandListImpl::cmd_build_blas(const BlasBuildDesc& desc) {
  device_.cmd_build_blas_impl(*this, desc);
}

void MetalCommandListImpl::cmd_write_blas_compacted_size(AccelStructHandle blas,
                                                         QueryPoolHandle pool,
                                                         uint32_t query_index) {
  device_.cmd_write_blas_compacted_size_impl(*this, blas, pool, query_index);
}

void MetalCommandListImpl::cmd_copy_compact_blas(AccelStructHandle src, AccelStructHandle dst) {
  device_.cmd_copy_compact_blas_impl(*this, src, dst);
}

void MetalCommandListImpl::cmd_build_tlas(const TlasBuildDesc& desc) {
  device_.cmd_build_tlas_impl(*this, desc);
}

// -----------------------------------------------------------------------------
// MetalSwapchainImpl (CAMetalLayer via SDL3 + Apple XDR EDR HDR)
// -----------------------------------------------------------------------------
MetalSwapchainImpl::MetalSwapchainImpl(MetalDeviceImpl& device) : device_(device) {}

MetalSwapchainImpl::~MetalSwapchainImpl() {
  @autoreleasepool {
    device_.wait_idle();
    for (FrameSlot& s : slots_) {
      release_slot_handles(s);
      s.cmd_list.reset();
    }
    slots_.clear();
    layer_ = nil;
#ifdef SHIFT_RHI_HAS_SDL3
    if (metal_view_) {
      SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metal_view_));
      metal_view_ = nullptr;
    }
#endif
    in_flight_sem_ = nil;
  }
}

void MetalSwapchainImpl::release_slot_handles(FrameSlot& slot) {
  if (slot.view_handle) {
    device_.unregister_external_texture_view(slot.view_handle);
    slot.view_handle = {};
  }
  if (slot.tex_handle) {
    device_.unregister_external_texture(slot.tex_handle);
    slot.tex_handle = {};
  }
  slot.drawable = nil;
}

bool MetalSwapchainImpl::init(const SwapchainDesc& desc, std::string& error) {
  @autoreleasepool {
    sdl_window_ = desc.sdl_window;
#ifdef SHIFT_RHI_HAS_SDL3
    if (!sdl_window_) {
      error = "create_swapchain(Metal): sdl_window is null";
      return false;
    }
    metal_view_ = SDL_Metal_CreateView(sdl_window_);
    if (!metal_view_) {
      error = std::string("SDL_Metal_CreateView failed: ") + SDL_GetError();
      return false;
    }
    layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(static_cast<SDL_MetalView>(metal_view_));
    if (!layer_) {
      error = "SDL_Metal_GetLayer returned nil CAMetalLayer";
      return false;
    }
#else
    error = "create_swapchain(Metal): SDL3 support is not compiled into libs/rhi";
    return false;
#endif

    layer_.device = device_.mtl_device();
    layer_.framebufferOnly = NO;

    const uint32_t fif = std::clamp(desc.frames_in_flight, 2u, 3u);
    drawable_count_ = fif;
    layer_.maximumDrawableCount = fif;
    in_flight_sem_ = dispatch_semaphore_create(static_cast<long>(fif));

    slots_.resize(fif);
    for (uint32_t i = 0; i < fif; ++i) {
      slots_[i].cmd_list = std::make_unique<MetalCommandListImpl>(device_, nil);
    }

    return configure_layer(desc.width, desc.height, desc.present_mode, desc.want_hdr, error);
  }
}

bool MetalSwapchainImpl::configure_layer(uint32_t req_width, uint32_t req_height,
                                         PresentMode mode, bool want_hdr, std::string& /*error*/) {
  @autoreleasepool {
    uint32_t w = req_width;
    uint32_t h = req_height;
#ifdef SHIFT_RHI_HAS_SDL3
    if ((w == 0 || h == 0) && sdl_window_) {
      int pw = 0, ph = 0;
      SDL_GetWindowSizeInPixels(sdl_window_, &pw, &ph);
      w = static_cast<uint32_t>(std::max(pw, 1));
      h = static_cast<uint32_t>(std::max(ph, 1));
    }
#endif
    width_ = w;
    height_ = h;
    if (width_ > 0 && height_ > 0) {
      layer_.drawableSize = CGSizeMake(static_cast<CGFloat>(width_), static_cast<CGFloat>(height_));
    }

#if TARGET_OS_OSX
    layer_.displaySyncEnabled = (mode != PresentMode::Immediate) ? YES : NO;
#endif
    active_present_mode_ = mode;

    // Native Apple XDR EDR HDR configuration (RGBA16Float + Extended Linear sRGB).
    if (want_hdr) {
      mtl_format_ = MTLPixelFormatRGBA16Float;
      format_ = Format::Rgba16Sfloat;
      layer_.pixelFormat = mtl_format_;
#if TARGET_OS_OSX
      layer_.wantsExtendedDynamicRangeContent = YES;
#endif
      CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
      if (cs) {
        layer_.colorspace = cs;
        CGColorSpaceRelease(cs);
      }
      active_hdr_mode_ = HdrMode::Edr;
    } else {
      mtl_format_ = MTLPixelFormatBGRA8Unorm;
      format_ = Format::Bgra8Unorm;
      layer_.pixelFormat = mtl_format_;
#if TARGET_OS_OSX
      layer_.wantsExtendedDynamicRangeContent = NO;
#endif
      CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      if (cs) {
        layer_.colorspace = cs;
        CGColorSpaceRelease(cs);
      }
      active_hdr_mode_ = HdrMode::Off;
    }
    return true;
  }
}

bool MetalSwapchainImpl::resize(uint32_t width, uint32_t height, PresentMode mode,
                                bool want_hdr, std::string& error) {
  device_.wait_idle();
  for (FrameSlot& s : slots_) release_slot_handles(s);
  return configure_layer(width, height, mode, want_hdr, error);
}

bool MetalSwapchainImpl::acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                                            bool& out_of_date, std::string& error) {
  out_of_date = false;
  if (!layer_ || width_ == 0 || height_ == 0) {
    out_of_date = true;
    return true;
  }

  @autoreleasepool {
    const dispatch_time_t wait_t =
        (timeout_ns == UINT64_MAX) ? DISPATCH_TIME_FOREVER
                                   : dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout_ns));
    if (dispatch_semaphore_wait(in_flight_sem_, wait_t) != 0) {
      return false;
    }

    FrameSlot& slot = slots_[current_slot_];
    release_slot_handles(slot);

    id<CAMetalDrawable> drawable = [layer_ nextDrawable];
    if (!drawable || !drawable.texture) {
      dispatch_semaphore_signal(in_flight_sem_);
      out_of_date = true;
      return true;
    }

    id<MTLCommandBuffer> cb = [device_.mtl_queue() commandBuffer];
    if (!cb) {
      dispatch_semaphore_signal(in_flight_sem_);
      error = "MetalSwapchain: MTLCommandQueue commandBuffer returned nil";
      return false;
    }

    slot.drawable = drawable;
    const uint32_t dw = static_cast<uint32_t>([drawable.texture width]);
    const uint32_t dh = static_cast<uint32_t>([drawable.texture height]);
    slot.tex_handle = device_.register_external_texture(drawable.texture, format_, dw, dh);
    slot.view_handle = device_.register_external_texture_view(slot.tex_handle, drawable.texture, format_);
    slot.cmd_list->reset_command_buffer(cb);

    out_frame.image_index = current_slot_;
    out_frame.frame_slot = current_slot_;
    out_frame.texture = slot.tex_handle;
    out_frame.view = slot.view_handle;
    out_frame.format = format_;
    out_frame.width = dw;
    out_frame.height = dh;
    out_frame.suboptimal = (dw != width_ || dh != height_);
    return true;
  }
}

CommandList& MetalSwapchainImpl::frame_command_list(uint32_t frame_slot) {
  return *slots_[frame_slot % slots_.size()].cmd_list;
}

bool MetalSwapchainImpl::submit_and_present(const SwapchainFrame& frame, bool& out_of_date,
                                            std::string& error) {
  out_of_date = false;
  if (frame.frame_slot >= slots_.size()) {
    error = "MetalSwapchain submit_and_present: invalid frame_slot";
    return false;
  }

  @autoreleasepool {
    FrameSlot& slot = slots_[frame.frame_slot];
    id<MTLCommandBuffer> cb = slot.cmd_list->native_cmd_buf();
    if (!cb) {
      dispatch_semaphore_signal(in_flight_sem_);
      error = "MetalSwapchain submit_and_present: command buffer is nil";
      return false;
    }

    dispatch_semaphore_t sem = in_flight_sem_;
    MetalDeviceImpl* dev_ptr = &device_;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      if ([completed status] == MTLCommandBufferStatusError) {
        dev_ptr->record_validation_message();
      }
      dispatch_semaphore_signal(sem);
    }];

    if (slot.drawable) {
      [cb presentDrawable:slot.drawable];
    }
    [cb commit];
    slot.drawable = nil;

    current_slot_ = (current_slot_ + 1u) % static_cast<uint32_t>(slots_.size());
    return true;
  }
}

}  // namespace shift::rhi::mtl
