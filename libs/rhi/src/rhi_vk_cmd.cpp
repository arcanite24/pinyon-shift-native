#include "rhi_vk_internal.h"

#include <algorithm>
#include <cstring>

#ifdef SHIFT_RHI_HAS_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

namespace shift::rhi::vk {

namespace {

std::string vk_err(const char* what, VkResult r) {
  return std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")";
}

}  // namespace

// -----------------------------------------------------------------------------
// VkCommandListImpl
// -----------------------------------------------------------------------------
VkCommandListImpl::VkCommandListImpl(VkDeviceImpl& device, VkCommandBuffer cmd)
    : device_(device), cmd_(cmd) {}

void VkCommandListImpl::begin() {
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &bi);
  current_layout_ = VK_NULL_HANDLE;
  current_bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
}

void VkCommandListImpl::end() {
  vkEndCommandBuffer(cmd_);
}

void VkCommandListImpl::barrier(const BarrierDesc& desc) {
  std::vector<VkMemoryBarrier2> mem_barriers;
  std::vector<VkBufferMemoryBarrier2> buf_barriers;
  std::vector<VkImageMemoryBarrier2> img_barriers;

  if (desc.global_compute_uav) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    mem_barriers.push_back(mb);
  }
  if (desc.global_as_build_to_read) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    mem_barriers.push_back(mb);
  }
  if (desc.global_host_read) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    mem_barriers.push_back(mb);
  }

  buf_barriers.reserve(desc.buffers.size());
  for (const BufferBarrier& b : desc.buffers) {
    const BufferEntry* be = device_.lookup_buffer(b.buffer);
    if (!be) continue;
    const StateTransitionInfo src = translate_resource_state(b.before, false, false);
    const StateTransitionInfo dst = translate_resource_state(b.after, false, true);
    VkBufferMemoryBarrier2 vb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    vb.srcStageMask = any_flag(b.src_stage) ? to_vk_stage2(b.src_stage) : src.stage;
    vb.srcAccessMask = any_flag(b.src_access) ? to_vk_access2(b.src_access) : src.access;
    vb.dstStageMask = any_flag(b.dst_stage) ? to_vk_stage2(b.dst_stage) : dst.stage;
    vb.dstAccessMask = any_flag(b.dst_access) ? to_vk_access2(b.dst_access) : dst.access;
    vb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vb.buffer = be->buffer;
    vb.offset = b.offset;
    vb.size = (b.size == UINT64_MAX) ? VK_WHOLE_SIZE : b.size;
    buf_barriers.push_back(vb);
  }

  img_barriers.reserve(desc.textures.size());
  for (const TextureBarrier& t : desc.textures) {
    const TextureEntry* te = device_.lookup_texture(t.texture);
    if (!te) continue;
    const bool is_depth = t.is_depth || format_is_depth(te->format);
    const StateTransitionInfo src = translate_resource_state(t.before, is_depth, false);
    const StateTransitionInfo dst = translate_resource_state(t.after, is_depth, true);
    VkImageMemoryBarrier2 vi{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    vi.srcStageMask = any_flag(t.src_stage) ? to_vk_stage2(t.src_stage) : src.stage;
    vi.srcAccessMask = any_flag(t.src_access) ? to_vk_access2(t.src_access) : src.access;
    vi.dstStageMask = any_flag(t.dst_stage) ? to_vk_stage2(t.dst_stage) : dst.stage;
    vi.dstAccessMask = any_flag(t.dst_access) ? to_vk_access2(t.dst_access) : dst.access;
    vi.oldLayout = src.layout;
    vi.newLayout = dst.layout;
    vi.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vi.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vi.image = te->image;
    vi.subresourceRange.aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = t.base_mip;
    vi.subresourceRange.levelCount = t.mip_count;
    vi.subresourceRange.baseArrayLayer = t.base_layer;
    vi.subresourceRange.layerCount = t.layer_count;
    img_barriers.push_back(vi);
  }

  if (mem_barriers.empty() && buf_barriers.empty() && img_barriers.empty()) return;

  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = static_cast<uint32_t>(mem_barriers.size());
  dep.pMemoryBarriers = mem_barriers.data();
  dep.bufferMemoryBarrierCount = static_cast<uint32_t>(buf_barriers.size());
  dep.pBufferMemoryBarriers = buf_barriers.data();
  dep.imageMemoryBarrierCount = static_cast<uint32_t>(img_barriers.size());
  dep.pImageMemoryBarriers = img_barriers.data();
  vkCmdPipelineBarrier2(cmd_, &dep);
}

void VkCommandListImpl::reset_query_pool(QueryPoolHandle pool, uint32_t first, uint32_t count) {
  const QueryPoolEntry* qe = device_.lookup_query_pool(pool);
  if (!qe || count == 0) return;
  vkCmdResetQueryPool(cmd_, qe->pool, first, count);
}

void VkCommandListImpl::write_timestamp(QueryPoolHandle pool, uint32_t index, PipelineStage stage) {
  const QueryPoolEntry* qe = device_.lookup_query_pool(pool);
  if (!qe || index >= qe->count) return;
  vkCmdWriteTimestamp2(cmd_, to_vk_stage2(stage), qe->pool, index);
}

void VkCommandListImpl::begin_render_pass(const RenderPassDesc& desc) {
  std::vector<VkRenderingAttachmentInfo> colors;
  colors.reserve(desc.color_attachments.size());
  for (const ColorAttachmentDesc& ca : desc.color_attachments) {
    const TextureViewEntry* tv = device_.lookup_texture_view(ca.view);
    if (!tv) continue;
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = tv->view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = to_vk_load_op(ca.load_op);
    att.storeOp = to_vk_store_op(ca.store_op);
    std::memcpy(att.clearValue.color.float32, ca.clear_color, sizeof(float) * 4);
    if (ca.resolve_view && ca.resolve_mode != ResolveMode::None) {
      if (const TextureViewEntry* rtv = device_.lookup_texture_view(ca.resolve_view)) {
        att.resolveMode = to_vk_resolve_mode(ca.resolve_mode);
        att.resolveImageView = rtv->view;
        att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      }
    }
    colors.push_back(att);
  }

  VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  bool has_depth = false;
  if (desc.depth_attachment && desc.depth_attachment->view) {
    const DepthAttachmentDesc& da = *desc.depth_attachment;
    if (const TextureViewEntry* dtv = device_.lookup_texture_view(da.view)) {
      has_depth = true;
      depth_att.imageView = dtv->view;
      depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
      depth_att.loadOp = to_vk_load_op(da.load_op);
      depth_att.storeOp = to_vk_store_op(da.store_op);
      depth_att.clearValue.depthStencil.depth = da.clear_depth;
      depth_att.clearValue.depthStencil.stencil = 0;
      if (da.resolve_view && da.resolve_mode != ResolveMode::None && device_.caps().depth_resolve_sample_zero) {
        if (const TextureViewEntry* rdv = device_.lookup_texture_view(da.resolve_view)) {
          depth_att.resolveMode = to_vk_resolve_mode(da.resolve_mode);
          depth_att.resolveImageView = rdv->view;
          depth_att.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }
      }
    }
  }

  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea.offset = {0, 0};
  ri.renderArea.extent = {desc.width, desc.height};
  ri.layerCount = 1;
  ri.colorAttachmentCount = static_cast<uint32_t>(colors.size());
  ri.pColorAttachments = colors.data();
  ri.pDepthAttachment = has_depth ? &depth_att : nullptr;
  vkCmdBeginRendering(cmd_, &ri);
}

void VkCommandListImpl::end_render_pass() {
  vkCmdEndRendering(cmd_);
}

void VkCommandListImpl::set_viewport(float x, float y, float width, float height,
                                     float min_depth, float max_depth) {
  VkViewport vp{x, y, width, height, min_depth, max_depth};
  vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VkCommandListImpl::set_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  VkRect2D sc{{x, y}, {width, height}};
  vkCmdSetScissor(cmd_, 0, 1, &sc);
}

void VkCommandListImpl::set_depth_bias(float constant_factor, float clamp, float slope_factor) {
  vkCmdSetDepthBias(cmd_, constant_factor, clamp, slope_factor);
}

void VkCommandListImpl::set_blend_constants(const float rgba[4]) {
  vkCmdSetBlendConstants(cmd_, rgba);
}

void VkCommandListImpl::set_shading_rate(uint32_t width, uint32_t height) {
  if (!device_.caps().fragment_shading_rate || !vkCmdSetFragmentShadingRateKHR) return;
  const VkExtent2D frag_size{width, height};
  const VkFragmentShadingRateCombinerOpKHR ops[2] = {
      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
  };
  vkCmdSetFragmentShadingRateKHR(cmd_, &frag_size, ops);
}

void VkCommandListImpl::bind_pipeline(PipelineHandle pipeline) {
  const PipelineEntry* pe = device_.lookup_pipeline(pipeline);
  if (!pe) return;
  vkCmdBindPipeline(cmd_, pe->bind_point, pe->pipeline);
  current_layout_ = pe->layout;
  current_bind_point_ = pe->bind_point;
}

void VkCommandListImpl::bind_group(uint32_t set_index, BindGroupHandle group) {
  if (!current_layout_) return;
  const BindGroupEntry* ge = device_.lookup_bind_group(group);
  if (!ge) return;
  vkCmdBindDescriptorSets(cmd_, current_bind_point_, current_layout_, set_index, 1, &ge->set, 0, nullptr);
}

void VkCommandListImpl::bind_bindless(uint32_t set_index, const BindlessTable& table) {
  bind_group(set_index, table.bind_group());
}

void VkCommandListImpl::push_constants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) {
  if (!current_layout_ || !data || size == 0) return;
  vkCmdPushConstants(cmd_, current_layout_, to_vk_shader_stages(stages), offset, size, data);
}

void VkCommandListImpl::bind_vertex_buffers(uint32_t first_binding,
                                            std::span<const BufferHandle> buffers,
                                            std::span<const uint64_t> offsets) {
  if (buffers.empty()) return;
  std::vector<VkBuffer> vk_bufs;
  std::vector<VkDeviceSize> vk_offs;
  vk_bufs.reserve(buffers.size());
  vk_offs.reserve(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    const BufferEntry* be = device_.lookup_buffer(buffers[i]);
    if (!be) return;
    vk_bufs.push_back(be->buffer);
    vk_offs.push_back(i < offsets.size() ? offsets[i] : 0ull);
  }
  vkCmdBindVertexBuffers(cmd_, first_binding, static_cast<uint32_t>(vk_bufs.size()),
                         vk_bufs.data(), vk_offs.data());
}

void VkCommandListImpl::bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type) {
  const BufferEntry* be = device_.lookup_buffer(buffer);
  if (!be) return;
  vkCmdBindIndexBuffer(cmd_, be->buffer, offset,
                       type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VkCommandListImpl::draw(uint32_t vertex_count, uint32_t instance_count,
                             uint32_t first_vertex, uint32_t first_instance) {
  vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
}

void VkCommandListImpl::draw_indexed(uint32_t index_count, uint32_t instance_count,
                                     uint32_t first_index, int32_t vertex_offset,
                                     uint32_t first_instance) {
  vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VkCommandListImpl::draw_indirect(BufferHandle arg_buffer, uint64_t arg_offset,
                                      uint32_t draw_count, uint32_t stride) {
  const BufferEntry* be = device_.lookup_buffer(arg_buffer);
  if (!be) return;
  vkCmdDrawIndirect(cmd_, be->buffer, arg_offset, draw_count, stride);
}

void VkCommandListImpl::draw_indexed_indirect_count(BufferHandle arg_buffer, uint64_t arg_offset,
                                                    BufferHandle count_buffer, uint64_t count_offset,
                                                    uint32_t max_draw_count, uint32_t stride) {
  if (!device_.caps().indirect_count || !vkCmdDrawIndexedIndirectCount) return;
  const BufferEntry* ab = device_.lookup_buffer(arg_buffer);
  const BufferEntry* cb = device_.lookup_buffer(count_buffer);
  if (!ab || !cb) return;
  vkCmdDrawIndexedIndirectCount(cmd_, ab->buffer, arg_offset, cb->buffer, count_offset, max_draw_count, stride);
}

void VkCommandListImpl::dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z) {
  vkCmdDispatch(cmd_, group_x, group_y, group_z);
}

void VkCommandListImpl::update_buffer(BufferHandle dst, uint64_t offset, uint64_t size, const void* data) {
  const BufferEntry* be = device_.lookup_buffer(dst);
  if (!be || !data || size == 0) return;
  vkCmdUpdateBuffer(cmd_, be->buffer, offset, size, data);
}

void VkCommandListImpl::fill_buffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
  const BufferEntry* be = device_.lookup_buffer(dst);
  if (!be) return;
  vkCmdFillBuffer(cmd_, be->buffer, offset, size == UINT64_MAX ? VK_WHOLE_SIZE : size, value);
}

void VkCommandListImpl::copy_buffer(BufferHandle src, uint64_t src_offset,
                                    BufferHandle dst, uint64_t dst_offset, uint64_t size) {
  const BufferEntry* sb = device_.lookup_buffer(src);
  const BufferEntry* db = device_.lookup_buffer(dst);
  if (!sb || !db || size == 0) return;
  VkBufferCopy region{src_offset, dst_offset, size};
  vkCmdCopyBuffer(cmd_, sb->buffer, db->buffer, 1, &region);
}

void VkCommandListImpl::copy_buffer_to_texture(BufferHandle src, TextureHandle dst,
                                               std::span<const BufferTextureCopy> regions) {
  const BufferEntry* sb = device_.lookup_buffer(src);
  const TextureEntry* dt = device_.lookup_texture(dst);
  if (!sb || !dt || regions.empty()) return;
  const bool tex_is_depth = format_is_depth(dt->format);
  std::vector<VkBufferImageCopy> vk_regions;
  vk_regions.reserve(regions.size());
  for (const BufferTextureCopy& r : regions) {
    VkBufferImageCopy c{};
    c.bufferOffset = r.buffer_offset;
    c.bufferRowLength = r.buffer_row_length;
    c.bufferImageHeight = r.buffer_image_height;
    c.imageSubresource.aspectMask =
        (r.is_depth || tex_is_depth) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.mipLevel = r.mip_level;
    c.imageSubresource.baseArrayLayer = r.base_layer;
    c.imageSubresource.layerCount = r.layer_count;
    c.imageOffset = {r.tex_x, r.tex_y, r.tex_z};
    c.imageExtent = {r.width, r.height, r.depth};
    vk_regions.push_back(c);
  }
  vkCmdCopyBufferToImage(cmd_, sb->buffer, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         static_cast<uint32_t>(vk_regions.size()), vk_regions.data());
}

void VkCommandListImpl::copy_texture_to_buffer(TextureHandle src, BufferHandle dst,
                                               std::span<const BufferTextureCopy> regions) {
  const TextureEntry* st = device_.lookup_texture(src);
  const BufferEntry* db = device_.lookup_buffer(dst);
  if (!st || !db || regions.empty()) return;
  const bool tex_is_depth = format_is_depth(st->format);
  std::vector<VkBufferImageCopy> vk_regions;
  vk_regions.reserve(regions.size());
  for (const BufferTextureCopy& r : regions) {
    VkBufferImageCopy c{};
    c.bufferOffset = r.buffer_offset;
    c.bufferRowLength = r.buffer_row_length;
    c.bufferImageHeight = r.buffer_image_height;
    c.imageSubresource.aspectMask =
        (r.is_depth || tex_is_depth) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.mipLevel = r.mip_level;
    c.imageSubresource.baseArrayLayer = r.base_layer;
    c.imageSubresource.layerCount = r.layer_count;
    c.imageOffset = {r.tex_x, r.tex_y, r.tex_z};
    c.imageExtent = {r.width, r.height, r.depth};
    vk_regions.push_back(c);
  }
  vkCmdCopyImageToBuffer(cmd_, st->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, db->buffer,
                         static_cast<uint32_t>(vk_regions.size()), vk_regions.data());
}

void VkCommandListImpl::copy_texture(TextureHandle src, TextureHandle dst, const TextureCopyRegion& r) {
  const TextureEntry* st = device_.lookup_texture(src);
  const TextureEntry* dt = device_.lookup_texture(dst);
  if (!st || !dt) return;
  const VkImageAspectFlags aspect =
      (r.is_depth || format_is_depth(st->format)) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageCopy copy{};
  copy.srcSubresource = {aspect, r.src_mip, r.src_layer, r.layer_count};
  copy.srcOffset = {r.src_x, r.src_y, 0};
  copy.dstSubresource = {aspect, r.dst_mip, r.dst_layer, r.layer_count};
  copy.dstOffset = {r.dst_x, r.dst_y, 0};
  copy.extent = {r.width, r.height, 1};
  vkCmdCopyImage(cmd_, st->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void VkCommandListImpl::blit(TextureHandle src, TextureHandle dst,
                             const TextureBlitRegion& r, Filter filter) {
  const TextureEntry* st = device_.lookup_texture(src);
  const TextureEntry* dt = device_.lookup_texture(dst);
  if (!st || !dt) return;
  const VkImageAspectFlags aspect =
      (r.is_depth || format_is_depth(st->format)) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageBlit b{};
  b.srcSubresource = {aspect, r.src_mip, r.src_layer, r.layer_count};
  b.srcOffsets[0] = {r.src_x0, r.src_y0, 0};
  b.srcOffsets[1] = {r.src_x1, r.src_y1, 1};
  b.dstSubresource = {aspect, r.dst_mip, r.dst_layer, r.layer_count};
  b.dstOffsets[0] = {r.dst_x0, r.dst_y0, 0};
  b.dstOffsets[1] = {r.dst_x1, r.dst_y1, 1};
  vkCmdBlitImage(cmd_, st->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, to_vk_filter(filter));
}

void VkCommandListImpl::clear_color_texture(TextureHandle dst, const float rgba[4]) {
  const TextureEntry* dt = device_.lookup_texture(dst);
  if (!dt) return;
  VkClearColorValue val{};
  std::memcpy(val.float32, rgba, sizeof(float) * 4);
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, dt->mip_levels, 0, dt->array_layers};
  vkCmdClearColorImage(cmd_, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &val, 1, &range);
}

void VkCommandListImpl::clear_depth_texture(TextureHandle dst, float depth) {
  const TextureEntry* dt = device_.lookup_texture(dst);
  if (!dt) return;
  VkClearDepthStencilValue val{depth, 0};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, dt->mip_levels, 0, dt->array_layers};
  vkCmdClearDepthStencilImage(cmd_, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &val, 1, &range);
}

void VkCommandListImpl::resolve_depth(TextureViewHandle msaa_depth, TextureViewHandle resolved_depth,
                                      uint32_t width, uint32_t height, ResolveMode mode) {
  if (!device_.caps().depth_resolve_sample_zero) return;
  const TextureViewEntry* src = device_.lookup_texture_view(msaa_depth);
  const TextureViewEntry* dst = device_.lookup_texture_view(resolved_depth);
  if (!src || !dst) return;

  VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth_att.imageView = src->view;
  depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth_att.resolveMode = to_vk_resolve_mode(mode == ResolveMode::None ? ResolveMode::SampleZero : mode);
  depth_att.resolveImageView = dst->view;
  depth_att.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea.offset = {0, 0};
  ri.renderArea.extent = {width, height};
  ri.layerCount = 1;
  ri.pDepthAttachment = &depth_att;
  vkCmdBeginRendering(cmd_, &ri);
  vkCmdEndRendering(cmd_);
}

void VkCommandListImpl::cmd_build_blas(const BlasBuildDesc& desc) {
  device_.cmd_build_blas_impl(cmd_, desc);
}

void VkCommandListImpl::cmd_write_blas_compacted_size(AccelStructHandle blas,
                                                      QueryPoolHandle pool, uint32_t query_index) {
  device_.cmd_write_blas_compacted_size_impl(cmd_, blas, pool, query_index);
}

void VkCommandListImpl::cmd_copy_compact_blas(AccelStructHandle src, AccelStructHandle dst) {
  device_.cmd_copy_compact_blas_impl(cmd_, src, dst);
}

void VkCommandListImpl::cmd_build_tlas(const TlasBuildDesc& desc) {
  device_.cmd_build_tlas_impl(cmd_, desc);
}

// -----------------------------------------------------------------------------
// VkSwapchainImpl (SDL3 surface + Android IDENTITY preTransform + HDR10/scRGB)
// -----------------------------------------------------------------------------
VkSwapchainImpl::VkSwapchainImpl(VkDeviceImpl& device) : device_(device) {}

VkSwapchainImpl::~VkSwapchainImpl() {
  VkDevice d = device_.vk_device();
  if (!d) return;
  device_.wait_idle();
  destroy_images_and_views();
  if (swapchain_) {
    vkDestroySwapchainKHR(d, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  for (FrameSync& f : frames_) {
    if (f.image_ready) vkDestroySemaphore(d, f.image_ready, nullptr);
    if (f.in_flight) vkDestroyFence(d, f.in_flight, nullptr);
  }
  frames_.clear();
  if (cmd_pool_) {
    vkDestroyCommandPool(d, cmd_pool_, nullptr);
    cmd_pool_ = VK_NULL_HANDLE;
  }
  if (owns_surface_ && surface_) {
    vkDestroySurfaceKHR(device_.vk_instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
}

void VkSwapchainImpl::destroy_images_and_views() {
  VkDevice d = device_.vk_device();
  for (TextureViewHandle vh : view_handles_) {
    if (const TextureViewEntry* tv = device_.lookup_texture_view(vh)) {
      if (tv->view) vkDestroyImageView(d, tv->view, nullptr);
    }
    device_.unregister_external_texture_view(vh);
  }
  for (TextureHandle th : texture_handles_) {
    device_.unregister_external_texture(th);
  }
  for (VkSemaphore s : render_done_) {
    if (s) vkDestroySemaphore(d, s, nullptr);
  }
  view_handles_.clear();
  texture_handles_.clear();
  render_done_.clear();
  images_.clear();
}

bool VkSwapchainImpl::init(const SwapchainDesc& desc, std::string& error) {
  sdl_window_ = desc.sdl_window;
  if (device_.probe_surface()) {
    surface_ = device_.probe_surface();
    owns_surface_ = false;
  } else {
#ifdef SHIFT_RHI_HAS_SDL3
    if (!sdl_window_) {
      error = "create_swapchain: sdl_window is null";
      return false;
    }
    if (!SDL_Vulkan_CreateSurface(sdl_window_, device_.vk_instance(), nullptr, &surface_)) {
      error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
      return false;
    }
    owns_surface_ = true;
#else
    error = "create_swapchain: SDL3 surface support is not compiled into libs/rhi";
    return false;
#endif
  }

  VkBool32 present_supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(device_.vk_physical(), device_.queue_family(),
                                       surface_, &present_supported);
  if (!present_supported) {
    error = "create_swapchain: selected queue family does not support presentation to this surface";
    return false;
  }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = device_.queue_family();
  VkResult r = vkCreateCommandPool(device_.vk_device(), &pci, nullptr, &cmd_pool_);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateCommandPool(swapchain)", r);
    return false;
  }

  const uint32_t fif = std::clamp(desc.frames_in_flight, 1u, 3u);
  frames_.resize(fif);
  for (uint32_t i = 0; i < fif; ++i) {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    r = vkCreateSemaphore(device_.vk_device(), &sci, nullptr, &frames_[i].image_ready);
    if (r != VK_SUCCESS) {
      error = vk_err("vkCreateSemaphore", r);
      return false;
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    r = vkCreateFence(device_.vk_device(), &fci, nullptr, &frames_[i].in_flight);
    if (r != VK_SUCCESS) {
      error = vk_err("vkCreateFence", r);
      return false;
    }
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(device_.vk_device(), &cbai, &frames_[i].cmd);
    if (r != VK_SUCCESS) {
      error = vk_err("vkAllocateCommandBuffers(swapchain)", r);
      return false;
    }
    frames_[i].wrapper = std::make_unique<VkCommandListImpl>(device_, frames_[i].cmd);
  }

  return rebuild_swapchain(desc.width, desc.height, desc.present_mode, desc.want_hdr, error);
}

bool VkSwapchainImpl::rebuild_swapchain(uint32_t req_width, uint32_t req_height,
                                        PresentMode mode, bool want_hdr, std::string& error) {
  VkPhysicalDevice pd = device_.vk_physical();
  VkDevice d = device_.vk_device();

  VkSurfaceCapabilitiesKHR caps{};
  VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface_, &caps);
  if (r != VK_SUCCESS) {
    error = vk_err("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    return false;
  }

  // Format + HDR10 / scRGB negotiation.
  uint32_t fmt_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &fmt_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmt_count);
  if (fmt_count > 0) vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &fmt_count, formats.data());

  VkFormat sdr_fmt = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
  bool has_hdr10 = false;
  bool has_scrgb = false;
  for (const VkSurfaceFormatKHR& sf : formats) {
    if ((sf.format == VK_FORMAT_B8G8R8A8_UNORM || sf.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      sdr_fmt = sf.format;
    }
    if (device_.hdr_colorspace_ext() &&
        sf.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
        sf.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
      has_hdr10 = true;
    }
    if (device_.hdr_colorspace_ext() &&
        sf.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
        sf.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
      has_scrgb = true;
    }
  }

  vk_format_ = sdr_fmt;
  colorspace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  active_hdr_mode_ = HdrMode::Off;
  if (want_hdr && has_scrgb) {
    vk_format_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorspace_ = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    active_hdr_mode_ = HdrMode::ScRgb;
  } else if (want_hdr && has_hdr10) {
    vk_format_ = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    colorspace_ = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    active_hdr_mode_ = HdrMode::Hdr10;
  }
  format_ = from_vk_format(vk_format_);

  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    uint32_t w = req_width;
    uint32_t h = req_height;
#ifdef SHIFT_RHI_HAS_SDL3
    if ((w == 0 || h == 0) && sdl_window_) {
      int sw = 0, sh = 0;
      SDL_GetWindowSizeInPixels(sdl_window_, &sw, &sh);
      w = static_cast<uint32_t>(std::max(sw, 1));
      h = static_cast<uint32_t>(std::max(sh, 1));
    }
#endif
    extent = {std::max(w, 1u), std::max(h, 1u)};
  }
  extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  if (extent.width == 0 || extent.height == 0) {
    extent_ = {0, 0};
    return true;
  }

  // Present mode negotiation (Immediate -> Mailbox -> Fifo fallback).
  uint32_t mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &mode_count, nullptr);
  std::vector<VkPresentModeKHR> modes(mode_count);
  if (mode_count > 0) vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &mode_count, modes.data());
  auto has_mode = [&](VkPresentModeKHR m) {
    return std::find(modes.begin(), modes.end(), m) != modes.end();
  };

  VkPresentModeKHR vk_mode = VK_PRESENT_MODE_FIFO_KHR;
  active_present_mode_ = PresentMode::Fifo;
  if (mode == PresentMode::Immediate && has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
    vk_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    active_present_mode_ = PresentMode::Immediate;
  } else if ((mode == PresentMode::Immediate || mode == PresentMode::Mailbox) &&
             has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) {
    vk_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    active_present_mode_ = PresentMode::Mailbox;
  }

  uint32_t image_count = caps.minImageCount + (active_present_mode_ == PresentMode::Fifo ? 1u : 2u);
  if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = image_count;
  sci.imageFormat = vk_format_;
  sci.imageColorSpace = colorspace_;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
    sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
    sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  // IDENTITY preTransform avoids rotated-surface stretching on Android.
  sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                         ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                         : caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = vk_mode;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;

  VkSwapchainKHR created = VK_NULL_HANDLE;
  r = vkCreateSwapchainKHR(d, &sci, nullptr, &created);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateSwapchainKHR", r);
    return false;
  }

  VkSwapchainKHR old = swapchain_;
  destroy_images_and_views();
  if (old) vkDestroySwapchainKHR(d, old, nullptr);
  swapchain_ = created;
  extent_ = extent;

  uint32_t actual_count = 0;
  vkGetSwapchainImagesKHR(d, swapchain_, &actual_count, nullptr);
  images_.resize(actual_count);
  vkGetSwapchainImagesKHR(d, swapchain_, &actual_count, images_.data());

  texture_handles_.reserve(actual_count);
  view_handles_.reserve(actual_count);
  render_done_.reserve(actual_count);
  for (VkImage img : images_) {
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = vk_format_;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    r = vkCreateImageView(d, &ivci, nullptr, &view);
    if (r != VK_SUCCESS) {
      error = vk_err("vkCreateImageView(swapchain)", r);
      return false;
    }
    VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore done = VK_NULL_HANDLE;
    r = vkCreateSemaphore(d, &sem_ci, nullptr, &done);
    if (r != VK_SUCCESS) {
      vkDestroyImageView(d, view, nullptr);
      error = vk_err("vkCreateSemaphore(swapchain)", r);
      return false;
    }
    TextureHandle th = device_.register_external_texture(img, vk_format_, extent_.width, extent_.height);
    TextureViewHandle vh = device_.register_external_texture_view(th, view, vk_format_);
    texture_handles_.push_back(th);
    view_handles_.push_back(vh);
    render_done_.push_back(done);
  }
  return true;
}

bool VkSwapchainImpl::resize(uint32_t width, uint32_t height, PresentMode mode,
                             bool want_hdr, std::string& error) {
  device_.wait_idle();
  return rebuild_swapchain(width, height, mode, want_hdr, error);
}

bool VkSwapchainImpl::acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                                         bool& out_of_date, std::string& error) {
  out_of_date = false;
  if (!swapchain_ || extent_.width == 0 || extent_.height == 0) {
    out_of_date = true;
    return true;
  }
  FrameSync& fs = frames_[current_slot_];
  vkWaitForFences(device_.vk_device(), 1, &fs.in_flight, VK_TRUE, UINT64_MAX);

  uint32_t img_idx = 0;
  const VkResult r = vkAcquireNextImageKHR(device_.vk_device(), swapchain_, timeout_ns,
                                           fs.image_ready, VK_NULL_HANDLE, &img_idx);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    out_of_date = true;
    return true;
  }
  if (r == VK_TIMEOUT || r == VK_NOT_READY) {
    return false;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    error = vk_err("vkAcquireNextImageKHR", r);
    return false;
  }

  vkResetFences(device_.vk_device(), 1, &fs.in_flight);
  vkResetCommandBuffer(fs.cmd, 0);
  fs.wrapper->reset_handle(fs.cmd);

  out_frame.image_index = img_idx;
  out_frame.frame_slot = current_slot_;
  out_frame.texture = texture_handles_[img_idx];
  out_frame.view = view_handles_[img_idx];
  out_frame.format = format_;
  out_frame.width = extent_.width;
  out_frame.height = extent_.height;
  out_frame.suboptimal = (r == VK_SUBOPTIMAL_KHR);
  return true;
}

CommandList& VkSwapchainImpl::frame_command_list(uint32_t frame_slot) {
  return *frames_[frame_slot % frames_.size()].wrapper;
}

bool VkSwapchainImpl::submit_and_present(const SwapchainFrame& frame, bool& out_of_date, std::string& error) {
  out_of_date = false;
  FrameSync& fs = frames_[frame.frame_slot % frames_.size()];
  VkSemaphore signal_sem = render_done_[frame.image_index];

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &fs.image_ready;
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &fs.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &signal_sem;

  VkResult r = VK_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(device_.queue_mutex());
    r = vkQueueSubmit(device_.graphics_queue(), 1, &si, fs.in_flight);
  }
  if (r != VK_SUCCESS) {
    error = vk_err("vkQueueSubmit(present)", r);
    return false;
  }

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &signal_sem;
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &frame.image_index;
  {
    std::lock_guard<std::mutex> lock(device_.queue_mutex());
    r = vkQueuePresentKHR(device_.graphics_queue(), &pi);
  }
  current_slot_ = (current_slot_ + 1u) % static_cast<uint32_t>(frames_.size());

  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    out_of_date = true;
    return true;
  }
  if (r != VK_SUCCESS) {
    error = vk_err("vkQueuePresentKHR", r);
    return false;
  }
  return true;
}

}  // namespace shift::rhi::vk
