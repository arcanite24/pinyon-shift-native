#include "rhi_d3d12_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef SHIFT_RHI_HAS_SDL3
#include <SDL3/SDL.h>
#endif

namespace shift::rhi::d3d12 {

namespace {

std::string hr_err(const char* what, HRESULT hr) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
  return std::string(what) + " failed (HRESULT " + buf + ")";
}

UINT calc_subresource(UINT mip_slice, UINT array_slice, UINT plane_slice,
                      UINT mip_levels, UINT array_size) noexcept {
  return mip_slice + array_slice * mip_levels + plane_slice * mip_levels * array_size;
}

}  // namespace

// -----------------------------------------------------------------------------
// D3D12CommandListImpl
// -----------------------------------------------------------------------------
D3D12CommandListImpl::D3D12CommandListImpl(D3D12DeviceImpl& device,
                                           ComPtr<ID3D12CommandAllocator> allocator,
                                           ComPtr<ID3D12GraphicsCommandList4> cmd,
                                           QueueKind queue_kind)
    : device_(device),
      queue_kind_(queue_kind),
      allocator_(std::move(allocator)),
      cmd_(std::move(cmd)) {
  if (cmd_) {
    cmd_.As(&cmd5_);
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    cmd_.As(&cmd7_);
#endif
  }
}

void D3D12CommandListImpl::reset_for_recording() {
  transient_resources_.clear();
  active_resolves_.clear();
  current_pipeline_ = {};
  current_layout_ = {};
  current_is_compute_ = false;
  bound_vb_mask_ = 0;
  vb_dirty_ = false;
  heaps_bound_ = false;
  recording_ = false;
}

void D3D12CommandListImpl::begin() {
  if (recording_) return;
  transient_resources_.clear();
  allocator_->Reset();
  cmd_->Reset(allocator_.Get(), nullptr);
  recording_ = true;
  heaps_bound_ = false;
  current_pipeline_ = {};
  current_layout_ = {};
  current_is_compute_ = false;
  bound_vb_mask_ = 0;
  vb_dirty_ = false;
  active_resolves_.clear();
  if (queue_kind_ != QueueKind::Transfer) {
    bind_global_heaps();
  }
}

void D3D12CommandListImpl::end() {
  if (!recording_) return;
  close_hr_ = cmd_->Close();
  recording_ = false;
}

void D3D12CommandListImpl::bind_global_heaps() {
  if (heaps_bound_) return;
  ID3D12DescriptorHeap* heaps[2] = {
      device_.cbv_srv_uav_heap().heap(),
      device_.sampler_heap().heap(),
  };
  cmd_->SetDescriptorHeaps(2, heaps);
  heaps_bound_ = true;
}

void D3D12CommandListImpl::transition_resource_legacy(ID3D12Resource* res,
                                                      D3D12_RESOURCE_STATES before,
                                                      D3D12_RESOURCE_STATES after) {
  if (!res || before == after) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  cmd_->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> D3D12CommandListImpl::allocate_transient_buffer(
    uint64_t size, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES init_state) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap_type;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = std::max<uint64_t>(size, 4ull);
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> res;
  if (FAILED(device_.d3d_device()->CreateCommittedResource(
          &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res)))) {
    return nullptr;
  }
  transient_resources_.push_back(res);
  return res;
}

void D3D12CommandListImpl::barrier(const BarrierDesc& desc) {
  // COPY command lists implicitly promote from COMMON on first copy and decay back
  // to COMMON at ExecuteCommandLists completion; non-copy resource transitions are
  // invalid on COPY queues in DirectX 12.
  if (queue_kind_ == QueueKind::Transfer) {
    for (const BufferBarrier& b : desc.buffers) {
      if (BufferEntry* be = device_.lookup_buffer(b.buffer)) {
        if (!be->fixed_state) be->current_state = D3D12_RESOURCE_STATE_COMMON;
      }
    }
    for (const TextureBarrier& t : desc.textures) {
      if (TextureEntry* te = device_.lookup_texture(t.texture)) {
        te->current_state = D3D12_RESOURCE_STATE_COMMON;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
        te->current_layout = D3D12_BARRIER_LAYOUT_COMMON;
#endif
      }
    }
    return;
  }

  auto sanitize_legacy_state = [&](D3D12_RESOURCE_STATES st) -> D3D12_RESOURCE_STATES {
    if (queue_kind_ == QueueKind::AsyncCompute) {
      st &= ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      if (st == D3D12_RESOURCE_STATE_RENDER_TARGET ||
          st == D3D12_RESOURCE_STATE_DEPTH_WRITE ||
          st == D3D12_RESOURCE_STATE_DEPTH_READ) {
        st = D3D12_RESOURCE_STATE_COMMON;
      }
    }
    return st;
  };

#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
  auto sanitize_sync = [&](D3D12_BARRIER_SYNC sync) -> D3D12_BARRIER_SYNC {
    if (queue_kind_ == QueueKind::AsyncCompute) {
      if (sync & D3D12_BARRIER_SYNC_ALL_SHADING) {
        sync &= ~D3D12_BARRIER_SYNC_ALL_SHADING;
        sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
      }
      constexpr D3D12_BARRIER_SYNC kGfxOnly =
          D3D12_BARRIER_SYNC_DRAW | D3D12_BARRIER_SYNC_INDEX_INPUT |
          D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_PIXEL_SHADING |
          D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_RENDER_TARGET |
          D3D12_BARRIER_SYNC_RESOLVE | D3D12_BARRIER_SYNC_ALL_SHADING;
      const D3D12_BARRIER_SYNC orig = sync;
      sync &= ~kGfxOnly;
      if (orig != D3D12_BARRIER_SYNC_NONE && sync == D3D12_BARRIER_SYNC_NONE) {
        sync = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
      }
    }
    return sync;
  };
  auto sanitize_access = [&](D3D12_BARRIER_ACCESS acc, D3D12_BARRIER_SYNC& sync) -> D3D12_BARRIER_ACCESS {
    if (queue_kind_ == QueueKind::AsyncCompute) {
      constexpr D3D12_BARRIER_ACCESS kGfxOnly =
          D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_INDEX_BUFFER |
          D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE |
          D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
      const D3D12_BARRIER_ACCESS orig = acc;
      acc &= ~kGfxOnly;
      if (orig != D3D12_BARRIER_ACCESS_COMMON && acc == D3D12_BARRIER_ACCESS_COMMON) {
        acc = D3D12_BARRIER_ACCESS_NO_ACCESS;
        sync = D3D12_BARRIER_SYNC_NONE;
      }
    }
    return acc;
  };

  if (device_.enhanced_barriers_enabled() && cmd7_) {
    std::vector<D3D12_GLOBAL_BARRIER> globals;
    std::vector<D3D12_BUFFER_BARRIER> buf_barriers;
    std::vector<D3D12_TEXTURE_BARRIER> tex_barriers;
    std::vector<D3D12_RESOURCE_BARRIER> legacy_fallback;

    if (desc.global_compute_uav) {
      D3D12_GLOBAL_BARRIER gb{};
      gb.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
      gb.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
      gb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
      gb.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
      globals.push_back(gb);
    }
    if (desc.global_as_build_to_read) {
      D3D12_GLOBAL_BARRIER gb{};
      gb.SyncBefore = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE |
                      D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE;
      gb.SyncAfter = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE |
                     D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE |
                     D3D12_BARRIER_SYNC_COMPUTE_SHADING |
                     D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO;
      gb.AccessBefore = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE |
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
      gb.AccessAfter = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ |
                       D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE |
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
      globals.push_back(gb);
    }
    // Note: global_host_read is satisfied by queue/fence completion on D3D12.

    for (const BufferBarrier& b : desc.buffers) {
      BufferEntry* be = device_.lookup_buffer(b.buffer);
      if (!be || !be->resource) continue;
      if (be->fixed_state) continue;
      D3D12_BUFFER_BARRIER bb{};
      bb.SyncBefore = sanitize_sync(to_d3d12_barrier_sync(b.before, b.src_stage));
      bb.SyncAfter = sanitize_sync(to_d3d12_barrier_sync(b.after, b.dst_stage));
      bb.AccessBefore = sanitize_access(to_d3d12_barrier_access(b.before, b.src_access, false), bb.SyncBefore);
      bb.AccessAfter = sanitize_access(to_d3d12_barrier_access(b.after, b.dst_access, false), bb.SyncAfter);
      if (bb.SyncBefore == D3D12_BARRIER_SYNC_NONE) bb.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
      if (bb.SyncAfter == D3D12_BARRIER_SYNC_NONE) bb.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
      bb.pResource = be->resource.Get();
      bb.Offset = 0;
      bb.Size = UINT64_MAX;
      buf_barriers.push_back(bb);
      be->current_state = sanitize_legacy_state(to_d3d12_resource_state(b.after, false));
    }

    for (const TextureBarrier& t : desc.textures) {
      TextureEntry* te = device_.lookup_texture(t.texture);
      if (!te || !te->resource) continue;
      const bool is_depth = t.is_depth || format_is_depth(te->format);
      if (te->enhanced_barrier_compatible) {
        D3D12_BARRIER_LAYOUT after_layout = to_d3d12_barrier_layout(t.after, is_depth);
        D3D12_BARRIER_LAYOUT before_layout = te->current_layout;
        if (queue_kind_ == QueueKind::AsyncCompute) {
          if (after_layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ) {
            after_layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
          } else if (after_layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
                     after_layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE) {
            after_layout = D3D12_BARRIER_LAYOUT_COMMON;
          }
          if (before_layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
              before_layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE ||
              before_layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ) {
            before_layout = D3D12_BARRIER_LAYOUT_COMMON;
          }
        }

        auto default_sync_access_for_layout = [&](D3D12_BARRIER_LAYOUT lay,
                                                  D3D12_BARRIER_SYNC& out_sync,
                                                  D3D12_BARRIER_ACCESS& out_acc) {
          switch (lay) {
            case D3D12_BARRIER_LAYOUT_UNDEFINED:
              out_sync = D3D12_BARRIER_SYNC_NONE;
              out_acc = D3D12_BARRIER_ACCESS_NO_ACCESS;
              break;
            case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
              out_sync = D3D12_BARRIER_SYNC_RENDER_TARGET;
              out_acc = D3D12_BARRIER_ACCESS_RENDER_TARGET;
              break;
            case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
              out_sync = D3D12_BARRIER_SYNC_ALL;
              out_acc = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
              break;
            case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
              out_sync = D3D12_BARRIER_SYNC_DEPTH_STENCIL;
              out_acc = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
              break;
            case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
              out_sync = D3D12_BARRIER_SYNC_DEPTH_STENCIL;
              out_acc = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
              break;
            case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
              out_sync = D3D12_BARRIER_SYNC_ALL_SHADING;
              out_acc = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
              break;
            case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
              out_sync = D3D12_BARRIER_SYNC_COPY;
              out_acc = D3D12_BARRIER_ACCESS_COPY_SOURCE;
              break;
            case D3D12_BARRIER_LAYOUT_COPY_DEST:
              out_sync = D3D12_BARRIER_SYNC_COPY;
              out_acc = D3D12_BARRIER_ACCESS_COPY_DEST;
              break;
            case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
              out_sync = D3D12_BARRIER_SYNC_RESOLVE;
              out_acc = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
              break;
            case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
              out_sync = D3D12_BARRIER_SYNC_RESOLVE;
              out_acc = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
              break;
            case D3D12_BARRIER_LAYOUT_COMMON:
            default:
              out_sync = D3D12_BARRIER_SYNC_ALL;
              out_acc = D3D12_BARRIER_ACCESS_COMMON;
              break;
          }
        };

        D3D12_TEXTURE_BARRIER tb{};
        if (t.before == ResourceState::Undefined) {
          default_sync_access_for_layout(before_layout, tb.SyncBefore, tb.AccessBefore);
        } else {
          tb.SyncBefore = to_d3d12_barrier_sync(t.before, t.src_stage);
          tb.AccessBefore = to_d3d12_barrier_access(t.before, t.src_access, is_depth);
        }
        tb.SyncBefore = sanitize_sync(tb.SyncBefore);
        tb.SyncAfter = sanitize_sync(to_d3d12_barrier_sync(t.after, t.dst_stage));
        tb.AccessBefore = sanitize_access(tb.AccessBefore, tb.SyncBefore);
        tb.AccessAfter = sanitize_access(to_d3d12_barrier_access(t.after, t.dst_access, is_depth),
                                         tb.SyncAfter);
        if (tb.SyncBefore == D3D12_BARRIER_SYNC_NONE) tb.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
        if (tb.SyncAfter == D3D12_BARRIER_SYNC_NONE) tb.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
        if (tb.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS) tb.SyncBefore = D3D12_BARRIER_SYNC_NONE;
        if (tb.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS) tb.SyncAfter = D3D12_BARRIER_SYNC_NONE;
        if (before_layout == D3D12_BARRIER_LAYOUT_UNDEFINED) {
          tb.SyncBefore = D3D12_BARRIER_SYNC_NONE;
          tb.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
        }
        if (tb.AccessBefore == D3D12_BARRIER_ACCESS_COMMON &&
            tb.SyncBefore != D3D12_BARRIER_SYNC_ALL) {
          tb.SyncBefore = D3D12_BARRIER_SYNC_ALL;
        }
        if (tb.AccessAfter == D3D12_BARRIER_ACCESS_COMMON &&
            tb.SyncAfter != D3D12_BARRIER_SYNC_ALL) {
          tb.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        }
        tb.LayoutBefore = before_layout;
        tb.LayoutAfter = after_layout;
        tb.pResource = te->resource.Get();
        tb.Subresources.IndexOrFirstMipLevel = t.base_mip;
        tb.Subresources.NumMipLevels = std::max(t.mip_count, 1u);
        tb.Subresources.FirstArraySlice = t.base_layer;
        tb.Subresources.NumArraySlices = std::max(t.layer_count, 1u);
        tb.Subresources.FirstPlane = 0;
        tb.Subresources.NumPlanes = 1;
        tb.Flags = (t.before == ResourceState::Undefined &&
                    before_layout == D3D12_BARRIER_LAYOUT_UNDEFINED)
                       ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                       : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        tex_barriers.push_back(tb);
        te->current_layout = after_layout;
        te->current_state = sanitize_legacy_state(to_d3d12_resource_state(t.after, is_depth));
      } else {
        const D3D12_RESOURCE_STATES after_state =
            sanitize_legacy_state(to_d3d12_resource_state(t.after, is_depth));
        if (te->current_state != after_state) {
          D3D12_RESOURCE_BARRIER rb{};
          rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          rb.Transition.pResource = te->resource.Get();
          rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          rb.Transition.StateBefore = te->current_state;
          rb.Transition.StateAfter = after_state;
          legacy_fallback.push_back(rb);
          te->current_state = after_state;
        }
      }
    }

    D3D12_BARRIER_GROUP groups[3]{};
    UINT group_count = 0;
    if (!globals.empty()) {
      groups[group_count].Type = D3D12_BARRIER_TYPE_GLOBAL;
      groups[group_count].NumBarriers = static_cast<UINT>(globals.size());
      groups[group_count].pGlobalBarriers = globals.data();
      ++group_count;
    }
    if (!buf_barriers.empty()) {
      groups[group_count].Type = D3D12_BARRIER_TYPE_BUFFER;
      groups[group_count].NumBarriers = static_cast<UINT>(buf_barriers.size());
      groups[group_count].pBufferBarriers = buf_barriers.data();
      ++group_count;
    }
    if (!tex_barriers.empty()) {
      groups[group_count].Type = D3D12_BARRIER_TYPE_TEXTURE;
      groups[group_count].NumBarriers = static_cast<UINT>(tex_barriers.size());
      groups[group_count].pTextureBarriers = tex_barriers.data();
      ++group_count;
    }
    if (group_count > 0) {
      cmd7_->Barrier(group_count, groups);
    }
    if (!legacy_fallback.empty()) {
      cmd_->ResourceBarrier(static_cast<UINT>(legacy_fallback.size()), legacy_fallback.data());
    }
    return;
  }
#endif

  // Legacy ResourceBarrier path (all D3D12 devices).
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  barriers.reserve(desc.buffers.size() + desc.textures.size() + 2);

  if (desc.global_compute_uav || desc.global_as_build_to_read) {
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nullptr;
    barriers.push_back(uav);
  }

  for (const BufferBarrier& b : desc.buffers) {
    BufferEntry* be = device_.lookup_buffer(b.buffer);
    if (!be || !be->resource) continue;
    if (be->fixed_state) continue;
    const D3D12_RESOURCE_STATES after_state =
        sanitize_legacy_state(to_d3d12_resource_state(b.after, false));
    if (be->current_state == after_state) {
      if (after_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = be->resource.Get();
        barriers.push_back(uav);
      }
      continue;
    }
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = be->resource.Get();
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = be->current_state;
    rb.Transition.StateAfter = after_state;
    barriers.push_back(rb);
    be->current_state = after_state;
  }

  for (const TextureBarrier& t : desc.textures) {
    TextureEntry* te = device_.lookup_texture(t.texture);
    if (!te || !te->resource) continue;
    const bool is_depth = t.is_depth || format_is_depth(te->format);
    const D3D12_RESOURCE_STATES after_state =
        sanitize_legacy_state(to_d3d12_resource_state(t.after, is_depth));
    if (te->current_state == after_state) {
      if (after_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = te->resource.Get();
        barriers.push_back(uav);
      }
      continue;
    }
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = te->resource.Get();
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = te->current_state;
    rb.Transition.StateAfter = after_state;
    barriers.push_back(rb);
    te->current_state = after_state;
  }

  if (!barriers.empty()) {
    cmd_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
  }
}

void D3D12CommandListImpl::reset_query_pool(QueryPoolHandle pool, uint32_t first, uint32_t count) {
  (void)pool;
  (void)first;
  (void)count;
  // D3D12 timestamp and postbuild-info queries do not require explicit reset commands.
}

void D3D12CommandListImpl::write_timestamp(QueryPoolHandle pool, uint32_t index, PipelineStage stage) {
  (void)stage;
  const QueryPoolEntry* qe = device_.lookup_query_pool(pool);
  if (!qe || qe->type != QueryType::Timestamp || !qe->heap || !qe->readback || index >= qe->count) {
    return;
  }
  cmd_->EndQuery(qe->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
  cmd_->ResolveQueryData(qe->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1,
                         qe->readback.Get(), static_cast<UINT64>(index) * sizeof(uint64_t));
}

void D3D12CommandListImpl::begin_render_pass(const RenderPassDesc& desc) {
  active_resolves_.clear();

  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]{};
  UINT rtv_count = 0;
  for (const ColorAttachmentDesc& ca : desc.color_attachments) {
    if (rtv_count >= 8) break;
    const TextureViewEntry* tv = device_.lookup_texture_view(ca.view);
    if (!tv || !tv->has_rtv) continue;
    const D3D12_CPU_DESCRIPTOR_HANDLE h = device_.rtv_heap().cpu_handle(tv->rtv_slot);
    rtvs[rtv_count++] = h;
    if (ca.load_op == LoadOp::Clear) {
      cmd_->ClearRenderTargetView(h, ca.clear_color, 0, nullptr);
    }
    if (ca.resolve_view && ca.resolve_mode != ResolveMode::None) {
      const TextureViewEntry* rtv_dst = device_.lookup_texture_view(ca.resolve_view);
      const TextureEntry* src_tex = device_.lookup_texture(tv->texture);
      const TextureEntry* dst_tex = rtv_dst ? device_.lookup_texture(rtv_dst->texture) : nullptr;
      if (src_tex && dst_tex) {
        PendingResolve pr{};
        pr.src_tex = tv->texture;
        pr.dst_tex = rtv_dst->texture;
        pr.src_subresource = calc_subresource(tv->base_mip, tv->base_layer, 0,
                                              src_tex->mip_levels, src_tex->array_layers);
        pr.dst_subresource = calc_subresource(rtv_dst->base_mip, rtv_dst->base_layer, 0,
                                              dst_tex->mip_levels, dst_tex->array_layers);
        pr.format = to_dxgi_format(dst_tex->format);
        pr.mode = ca.resolve_mode;
        pr.is_depth = false;
        active_resolves_.push_back(pr);
      }
    }
  }

  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  bool has_dsv = false;
  if (desc.depth_attachment && desc.depth_attachment->view) {
    const DepthAttachmentDesc& da = *desc.depth_attachment;
    const TextureViewEntry* dtv = device_.lookup_texture_view(da.view);
    if (dtv && dtv->has_dsv) {
      dsv = device_.dsv_heap().cpu_handle(dtv->dsv_slot);
      has_dsv = true;
      if (da.load_op == LoadOp::Clear) {
        cmd_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, da.clear_depth, 0, 0, nullptr);
      }
      if (da.resolve_view && da.resolve_mode != ResolveMode::None) {
        const TextureViewEntry* rdv = device_.lookup_texture_view(da.resolve_view);
        const TextureEntry* src_tex = device_.lookup_texture(dtv->texture);
        const TextureEntry* dst_tex = rdv ? device_.lookup_texture(rdv->texture) : nullptr;
        if (src_tex && dst_tex) {
          PendingResolve pr{};
          pr.src_tex = dtv->texture;
          pr.dst_tex = rdv->texture;
          pr.src_subresource = calc_subresource(dtv->base_mip, dtv->base_layer, 0,
                                                src_tex->mip_levels, src_tex->array_layers);
          pr.dst_subresource = calc_subresource(rdv->base_mip, rdv->base_layer, 0,
                                                dst_tex->mip_levels, dst_tex->array_layers);
          pr.format = to_dxgi_srv_format(dst_tex->format);
          pr.mode = da.resolve_mode;
          pr.is_depth = true;
          active_resolves_.push_back(pr);
        }
      }
    }
  }

  cmd_->OMSetRenderTargets(rtv_count, rtv_count > 0 ? rtvs : nullptr, FALSE,
                           has_dsv ? &dsv : nullptr);
}

void D3D12CommandListImpl::end_render_pass() {
  for (const PendingResolve& pr : active_resolves_) {
    TextureEntry* src = device_.lookup_texture(pr.src_tex);
    TextureEntry* dst = device_.lookup_texture(pr.dst_tex);
    if (!src || !dst || !src->resource || !dst->resource) continue;

    // Transition src -> RESOLVE_SOURCE and dst -> RESOLVE_DEST.
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    if (device_.enhanced_barriers_enabled() && cmd7_ &&
        src->enhanced_barrier_compatible && dst->enhanced_barrier_compatible) {
      D3D12_TEXTURE_BARRIER tb[2]{};
      tb[0].SyncBefore = D3D12_BARRIER_SYNC_ALL;
      tb[0].SyncAfter = D3D12_BARRIER_SYNC_RESOLVE;
      tb[0].AccessBefore = pr.is_depth ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE
                                       : D3D12_BARRIER_ACCESS_RENDER_TARGET;
      tb[0].AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
      tb[0].LayoutBefore = src->current_layout;
      tb[0].LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;
      tb[0].pResource = src->resource.Get();
      tb[0].Subresources.IndexOrFirstMipLevel = 0xffffffff;

      tb[1].SyncBefore = D3D12_BARRIER_SYNC_ALL;
      tb[1].SyncAfter = D3D12_BARRIER_SYNC_RESOLVE;
      tb[1].AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
      tb[1].AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
      tb[1].LayoutBefore = dst->current_layout;
      tb[1].LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
      tb[1].pResource = dst->resource.Get();
      tb[1].Subresources.IndexOrFirstMipLevel = 0xffffffff;

      D3D12_BARRIER_GROUP bg{D3D12_BARRIER_TYPE_TEXTURE, 2};
      bg.pTextureBarriers = tb;
      cmd7_->Barrier(1, &bg);

      if (pr.is_depth) {
        const D3D12_RESOLVE_MODE rm = (pr.mode == ResolveMode::Average)
                                          ? D3D12_RESOLVE_MODE_AVERAGE
                                          : D3D12_RESOLVE_MODE_MIN;
        cmd_->ResolveSubresourceRegion(dst->resource.Get(), pr.dst_subresource, 0, 0,
                                       src->resource.Get(), pr.src_subresource, nullptr,
                                       pr.format, rm);
      } else {
        cmd_->ResolveSubresource(dst->resource.Get(), pr.dst_subresource,
                                 src->resource.Get(), pr.src_subresource, pr.format);
      }

      // Restore layouts back to prior state.
      tb[0].SyncBefore = D3D12_BARRIER_SYNC_RESOLVE;
      tb[0].SyncAfter = D3D12_BARRIER_SYNC_ALL;
      tb[0].AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
      tb[0].AccessAfter = pr.is_depth ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE
                                      : D3D12_BARRIER_ACCESS_RENDER_TARGET;
      tb[0].LayoutAfter = src->current_layout;
      tb[0].LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;

      tb[1].SyncBefore = D3D12_BARRIER_SYNC_RESOLVE;
      tb[1].SyncAfter = D3D12_BARRIER_SYNC_ALL;
      tb[1].AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
      tb[1].AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
      tb[1].LayoutAfter = dst->current_layout;
      tb[1].LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
      cmd7_->Barrier(1, &bg);
      continue;
    }
#endif

    const D3D12_RESOURCE_STATES prev_src = src->current_state;
    const D3D12_RESOURCE_STATES prev_dst = dst->current_state;
    transition_resource_legacy(src->resource.Get(), prev_src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    transition_resource_legacy(dst->resource.Get(), prev_dst, D3D12_RESOURCE_STATE_RESOLVE_DEST);

    if (pr.is_depth) {
      const D3D12_RESOLVE_MODE rm = (pr.mode == ResolveMode::Average)
                                        ? D3D12_RESOLVE_MODE_AVERAGE
                                        : D3D12_RESOLVE_MODE_MIN;
      cmd_->ResolveSubresourceRegion(dst->resource.Get(), pr.dst_subresource, 0, 0,
                                     src->resource.Get(), pr.src_subresource, nullptr,
                                     pr.format, rm);
    } else {
      cmd_->ResolveSubresource(dst->resource.Get(), pr.dst_subresource,
                               src->resource.Get(), pr.src_subresource, pr.format);
    }

    transition_resource_legacy(src->resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, prev_src);
    transition_resource_legacy(dst->resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, prev_dst);
  }
  active_resolves_.clear();
}

void D3D12CommandListImpl::set_viewport(float x, float y, float width, float height,
                                        float min_depth, float max_depth) {
  D3D12_VIEWPORT vp{x, y, width, height, min_depth, max_depth};
  cmd_->RSSetViewports(1, &vp);
}

void D3D12CommandListImpl::set_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  D3D12_RECT rc{x, y, x + static_cast<LONG>(width), y + static_cast<LONG>(height)};
  cmd_->RSSetScissorRects(1, &rc);
}

void D3D12CommandListImpl::set_depth_bias(float constant_factor, float clamp, float slope_factor) {
  PipelineEntry* pe = device_.lookup_pipeline(current_pipeline_);
  if (!pe || pe->is_compute || !pe->dynamic_depth_bias) return;
  if (ID3D12PipelineState* variant =
          device_.get_or_create_bias_pso(*pe, constant_factor, clamp, slope_factor)) {
    cmd_->SetPipelineState(variant);
  }
}

void D3D12CommandListImpl::set_blend_constants(const float rgba[4]) {
  if (!rgba) return;
  cmd_->OMSetBlendFactor(rgba);
}

void D3D12CommandListImpl::set_shading_rate(uint32_t width, uint32_t height) {
  if (!device_.caps().fragment_shading_rate || !cmd5_) return;
  D3D12_SHADING_RATE rate = D3D12_SHADING_RATE_1X1;
  if (width == 2 && height == 1) rate = D3D12_SHADING_RATE_2X1;
  else if (width == 1 && height == 2) rate = D3D12_SHADING_RATE_1X2;
  else if (width == 2 && height == 2) rate = D3D12_SHADING_RATE_2X2;
  else if (width == 4 && height == 2) rate = D3D12_SHADING_RATE_4X2;
  else if (width == 2 && height == 4) rate = D3D12_SHADING_RATE_2X4;
  else if (width == 4 && height == 4) rate = D3D12_SHADING_RATE_4X4;
  D3D12_SHADING_RATE_COMBINER combiners[2] = {
      D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
      D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
  };
  cmd5_->RSSetShadingRate(rate, combiners);
}

void D3D12CommandListImpl::bind_pipeline(PipelineHandle pipeline) {
  const PipelineEntry* pe = device_.lookup_pipeline(pipeline);
  if (!pe || !pe->pso) return;
  bind_global_heaps();
  cmd_->SetPipelineState(pe->pso.Get());
  if (pe->is_compute) {
    cmd_->SetComputeRootSignature(pe->root_signature);
  } else {
    cmd_->SetGraphicsRootSignature(pe->root_signature);
    cmd_->IASetPrimitiveTopology(pe->topology);
  }
  current_pipeline_ = pipeline;
  current_layout_ = pe->layout_handle;
  current_is_compute_ = pe->is_compute;
  if (!pe->is_compute && bound_vb_mask_ != 0) vb_dirty_ = true;
}

void D3D12CommandListImpl::bind_group(uint32_t set_index, BindGroupHandle group) {
  const PipelineLayoutEntry* pl = device_.lookup_pipeline_layout(current_layout_);
  const BindGroupEntry* ge = device_.lookup_bind_group(group);
  if (!pl || !ge || set_index >= pl->set_mappings.size()) return;
  bind_global_heaps();

  const PipelineLayoutSetMapping& m = pl->set_mappings[set_index];
  if (m.cbv_srv_uav_root_param != UINT32_MAX && ge->cbv_srv_uav_base != UINT32_MAX) {
    const D3D12_GPU_DESCRIPTOR_HANDLE h =
        device_.cbv_srv_uav_heap().gpu_handle(ge->cbv_srv_uav_base);
    if (current_is_compute_) {
      cmd_->SetComputeRootDescriptorTable(m.cbv_srv_uav_root_param, h);
    } else {
      cmd_->SetGraphicsRootDescriptorTable(m.cbv_srv_uav_root_param, h);
    }
  }
  if (m.sampler_root_param != UINT32_MAX && ge->sampler_base != UINT32_MAX) {
    const D3D12_GPU_DESCRIPTOR_HANDLE h =
        device_.sampler_heap().gpu_handle(ge->sampler_base);
    if (current_is_compute_) {
      cmd_->SetComputeRootDescriptorTable(m.sampler_root_param, h);
    } else {
      cmd_->SetGraphicsRootDescriptorTable(m.sampler_root_param, h);
    }
  }
}

void D3D12CommandListImpl::bind_bindless(uint32_t set_index, const BindlessTable& table) {
  bind_group(set_index, table.bind_group());
}

void D3D12CommandListImpl::push_constants(ShaderStage stages, uint32_t offset,
                                          uint32_t size, const void* data) {
  (void)stages;
  if (!data || size == 0) return;
  const PipelineLayoutEntry* pl = device_.lookup_pipeline_layout(current_layout_);
  if (!pl || pl->push_const_root_param == UINT32_MAX) return;
  const uint32_t first_dword = offset / 4u;
  const uint32_t num_dwords = std::min((size + 3u) / 4u,
                                       pl->push_const_dwords > first_dword
                                           ? pl->push_const_dwords - first_dword
                                           : 0u);
  if (num_dwords == 0) return;
  if (current_is_compute_) {
    cmd_->SetComputeRoot32BitConstants(pl->push_const_root_param, num_dwords, data, first_dword);
  } else {
    cmd_->SetGraphicsRoot32BitConstants(pl->push_const_root_param, num_dwords, data, first_dword);
  }
}

void D3D12CommandListImpl::bind_vertex_buffers(uint32_t first_binding,
                                               std::span<const BufferHandle> buffers,
                                               std::span<const uint64_t> offsets) {
  for (size_t i = 0; i < buffers.size(); ++i) {
    const uint32_t slot = first_binding + static_cast<uint32_t>(i);
    if (slot >= bound_vbs_.size()) break;
    bound_vbs_[slot].handle = buffers[i];
    bound_vbs_[slot].offset = (i < offsets.size()) ? offsets[i] : 0ull;
    bound_vb_mask_ |= (1u << slot);
  }
  vb_dirty_ = true;
}

void D3D12CommandListImpl::flush_vertex_buffers() {
  if (!vb_dirty_ || bound_vb_mask_ == 0) return;
  const PipelineEntry* pe = device_.lookup_pipeline(current_pipeline_);
  for (uint32_t slot = 0; slot < static_cast<uint32_t>(bound_vbs_.size()); ++slot) {
    if (!(bound_vb_mask_ & (1u << slot))) continue;
    const BufferEntry* be = device_.lookup_buffer(bound_vbs_[slot].handle);
    if (!be || !be->resource) continue;
    const uint64_t off = std::min(bound_vbs_[slot].offset, be->allocated_size);
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = be->gpu_va + off;
    vbv.SizeInBytes = static_cast<UINT>(be->allocated_size - off);
    vbv.StrideInBytes = pe ? pe->vertex_strides[slot] : 0u;
    cmd_->IASetVertexBuffers(slot, 1, &vbv);
  }
  vb_dirty_ = false;
}

void D3D12CommandListImpl::bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type) {
  const BufferEntry* be = device_.lookup_buffer(buffer);
  if (!be || !be->resource) return;
  const uint64_t off = std::min(offset, be->allocated_size);
  D3D12_INDEX_BUFFER_VIEW ibv{};
  ibv.BufferLocation = be->gpu_va + off;
  ibv.SizeInBytes = static_cast<UINT>(be->allocated_size - off);
  ibv.Format = (type == IndexType::Uint16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
  cmd_->IASetIndexBuffer(&ibv);
}

void D3D12CommandListImpl::draw(uint32_t vertex_count, uint32_t instance_count,
                                uint32_t first_vertex, uint32_t first_instance) {
  flush_vertex_buffers();
  cmd_->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
}

void D3D12CommandListImpl::draw_indexed(uint32_t index_count, uint32_t instance_count,
                                        uint32_t first_index, int32_t vertex_offset,
                                        uint32_t first_instance) {
  flush_vertex_buffers();
  cmd_->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
}

void D3D12CommandListImpl::draw_indirect(BufferHandle arg_buffer, uint64_t arg_offset,
                                         uint32_t draw_count, uint32_t stride) {
  if (draw_count == 0) return;
  const BufferEntry* arg = device_.lookup_buffer(arg_buffer);
  if (!arg || !arg->resource) return;
  const uint32_t eff_stride = (stride >= 16u) ? stride : 16u;
  ID3D12CommandSignature* sig = device_.get_or_create_indirect_signature(false, eff_stride);
  if (!sig) return;
  flush_vertex_buffers();
  cmd_->ExecuteIndirect(sig, draw_count, arg->resource.Get(), arg_offset, nullptr, 0);
}

void D3D12CommandListImpl::draw_indexed_indirect_count(BufferHandle arg_buffer, uint64_t arg_offset,
                                                       BufferHandle count_buffer, uint64_t count_offset,
                                                       uint32_t max_draw_count, uint32_t stride) {
  if (max_draw_count == 0) return;
  const BufferEntry* arg = device_.lookup_buffer(arg_buffer);
  const BufferEntry* cnt = device_.lookup_buffer(count_buffer);
  if (!arg || !arg->resource || !cnt || !cnt->resource) return;
  const uint32_t eff_stride = (stride >= 20u) ? stride : 20u;
  ID3D12CommandSignature* sig = device_.get_or_create_indirect_signature(true, eff_stride);
  if (!sig) return;
  flush_vertex_buffers();
  cmd_->ExecuteIndirect(sig, max_draw_count, arg->resource.Get(), arg_offset,
                        cnt->resource.Get(), count_offset);
}

void D3D12CommandListImpl::dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z) {
  cmd_->Dispatch(group_x, group_y, group_z);
}

void D3D12CommandListImpl::dispatch_indirect(BufferHandle arg_buffer, uint64_t arg_offset) {
  const BufferEntry* arg = device_.lookup_buffer(arg_buffer);
  if (!arg || !arg->resource) return;
  ID3D12CommandSignature* sig = device_.get_or_create_dispatch_indirect_signature();
  if (!sig) return;
  cmd_->ExecuteIndirect(sig, 1, arg->resource.Get(), arg_offset, nullptr, 0);
}

// -----------------------------------------------------------------------------
// Transfers, clears, blits, and explicit depth resolve
// -----------------------------------------------------------------------------
void D3D12CommandListImpl::update_buffer(BufferHandle dst, uint64_t offset,
                                         uint64_t size, const void* data) {
  if (!data || size == 0) return;
  const BufferEntry* dst_e = device_.lookup_buffer(dst);
  if (!dst_e || !dst_e->resource) return;
  ComPtr<ID3D12Resource> staging =
      allocate_transient_buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!staging) return;
  void* mapped = nullptr;
  const D3D12_RANGE empty{0, 0};
  if (SUCCEEDED(staging->Map(0, &empty, &mapped)) && mapped) {
    std::memcpy(mapped, data, static_cast<size_t>(size));
    staging->Unmap(0, nullptr);
    cmd_->CopyBufferRegion(dst_e->resource.Get(), offset, staging.Get(), 0, size);
  }
}

void D3D12CommandListImpl::fill_buffer(BufferHandle dst, uint64_t offset,
                                       uint64_t size, uint32_t value) {
  if (size == 0) return;
  const BufferEntry* dst_e = device_.lookup_buffer(dst);
  if (!dst_e || !dst_e->resource || offset >= dst_e->allocated_size) return;
  const uint64_t max_bytes = dst_e->allocated_size - offset;
  const uint64_t eff_size = (size == UINT64_MAX) ? max_bytes : std::min(size, max_bytes);
  if (eff_size == 0) return;
  const uint64_t aligned_size = align_up(eff_size, 4ull);
  ComPtr<ID3D12Resource> staging = allocate_transient_buffer(
      aligned_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!staging) return;
  void* mapped = nullptr;
  const D3D12_RANGE empty{0, 0};
  if (SUCCEEDED(staging->Map(0, &empty, &mapped)) && mapped) {
    auto* words = static_cast<uint32_t*>(mapped);
    const size_t count = static_cast<size_t>(aligned_size / 4ull);
    for (size_t i = 0; i < count; ++i) words[i] = value;
    staging->Unmap(0, nullptr);
    cmd_->CopyBufferRegion(dst_e->resource.Get(), offset, staging.Get(), 0, eff_size);
  }
}

void D3D12CommandListImpl::copy_buffer(BufferHandle src, uint64_t src_offset,
                                       BufferHandle dst, uint64_t dst_offset, uint64_t size) {
  if (size == 0) return;
  const BufferEntry* s = device_.lookup_buffer(src);
  const BufferEntry* d = device_.lookup_buffer(dst);
  if (!s || !d || !s->resource || !d->resource) return;
  cmd_->CopyBufferRegion(d->resource.Get(), dst_offset, s->resource.Get(), src_offset, size);
}

void D3D12CommandListImpl::copy_buffer_to_texture(BufferHandle src, TextureHandle dst,
                                                  std::span<const BufferTextureCopy> regions) {
  const BufferEntry* s = device_.lookup_buffer(src);
  const TextureEntry* d = device_.lookup_texture(dst);
  if (!s || !d || !s->resource || !d->resource) return;

  const uint32_t bpb = format_bytes_per_block(d->format);
  const uint32_t bext = format_block_extent(d->format);
  if (bpb == 0 || bext == 0) return;

  for (const BufferTextureCopy& r : regions) {
    const uint32_t w = r.width ? r.width : std::max(d->width >> r.mip_level, 1u);
    const uint32_t h = r.height ? r.height : std::max(d->height >> r.mip_level, 1u);
    const uint32_t depth = std::max(r.depth, 1u);
    const uint32_t row_pixels = r.buffer_row_length ? r.buffer_row_length : w;
    const uint32_t blocks_x = (row_pixels + bext - 1u) / bext;
    const uint32_t blocks_y = (h + bext - 1u) / bext;
    const uint64_t src_row_pitch = static_cast<uint64_t>(blocks_x) * bpb;
    const uint64_t aligned_pitch = align_up(src_row_pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t layer_count = std::max(r.layer_count, 1u);

    const bool direct_ok =
        (src_row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0) &&
        (r.buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0) &&
        (layer_count == 1);

    for (uint32_t layer = 0; layer < layer_count; ++layer) {
      const uint64_t slice_src_offset =
          r.buffer_offset + static_cast<uint64_t>(layer) * src_row_pitch * blocks_y * depth;
      D3D12_TEXTURE_COPY_LOCATION dst_loc{};
      dst_loc.pResource = d->resource.Get();
      dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst_loc.SubresourceIndex = calc_subresource(
          r.mip_level, r.base_layer + layer, 0, d->mip_levels, d->array_layers);

      if (direct_ok) {
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = s->resource.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset = slice_src_offset;
        src_loc.PlacedFootprint.Footprint.Format = d->copy_format;
        src_loc.PlacedFootprint.Footprint.Width = w;
        src_loc.PlacedFootprint.Footprint.Height = h;
        src_loc.PlacedFootprint.Footprint.Depth = depth;
        src_loc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(src_row_pitch);
        cmd_->CopyTextureRegion(&dst_loc, static_cast<UINT>(r.tex_x),
                                static_cast<UINT>(r.tex_y), static_cast<UINT>(r.tex_z),
                                &src_loc, nullptr);
      } else {
        // Repack rows on GPU into a 256-byte row-pitch aligned transient DEFAULT buffer.
        const uint64_t total_rows = static_cast<uint64_t>(blocks_y) * depth;
        const uint64_t padded_bytes = aligned_pitch * total_rows;
        ComPtr<ID3D12Resource> padded = allocate_transient_buffer(
            padded_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!padded) continue;
        for (uint64_t row = 0; row < total_rows; ++row) {
          cmd_->CopyBufferRegion(padded.Get(), row * aligned_pitch,
                                 s->resource.Get(), slice_src_offset + row * src_row_pitch,
                                 src_row_pitch);
        }
        transition_resource_legacy(padded.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = padded.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset = 0;
        src_loc.PlacedFootprint.Footprint.Format = d->copy_format;
        src_loc.PlacedFootprint.Footprint.Width = w;
        src_loc.PlacedFootprint.Footprint.Height = h;
        src_loc.PlacedFootprint.Footprint.Depth = depth;
        src_loc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(aligned_pitch);
        cmd_->CopyTextureRegion(&dst_loc, static_cast<UINT>(r.tex_x),
                                static_cast<UINT>(r.tex_y), static_cast<UINT>(r.tex_z),
                                &src_loc, nullptr);
      }
    }
  }
}

void D3D12CommandListImpl::copy_texture_to_buffer(TextureHandle src, BufferHandle dst,
                                                  std::span<const BufferTextureCopy> regions) {
  const TextureEntry* s = device_.lookup_texture(src);
  const BufferEntry* d = device_.lookup_buffer(dst);
  if (!s || !d || !s->resource || !d->resource) return;

  const uint32_t bpb = format_bytes_per_block(s->format);
  const uint32_t bext = format_block_extent(s->format);
  if (bpb == 0 || bext == 0) return;

  for (const BufferTextureCopy& r : regions) {
    const uint32_t w = r.width ? r.width : std::max(s->width >> r.mip_level, 1u);
    const uint32_t h = r.height ? r.height : std::max(s->height >> r.mip_level, 1u);
    const uint32_t depth = std::max(r.depth, 1u);
    const uint32_t row_pixels = r.buffer_row_length ? r.buffer_row_length : w;
    const uint32_t blocks_x = (row_pixels + bext - 1u) / bext;
    const uint32_t blocks_y = (h + bext - 1u) / bext;
    const uint64_t dst_row_pitch = static_cast<uint64_t>(blocks_x) * bpb;
    const uint64_t aligned_pitch = align_up(dst_row_pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t layer_count = std::max(r.layer_count, 1u);

    const bool direct_ok =
        (dst_row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0) &&
        (r.buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0) &&
        (layer_count == 1);

    D3D12_BOX box{};
    box.left = static_cast<UINT>(r.tex_x);
    box.top = static_cast<UINT>(r.tex_y);
    box.front = static_cast<UINT>(r.tex_z);
    box.right = static_cast<UINT>(r.tex_x) + w;
    box.bottom = static_cast<UINT>(r.tex_y) + h;
    box.back = static_cast<UINT>(r.tex_z) + depth;

    for (uint32_t layer = 0; layer < layer_count; ++layer) {
      const uint64_t slice_dst_offset =
          r.buffer_offset + static_cast<uint64_t>(layer) * dst_row_pitch * blocks_y * depth;
      D3D12_TEXTURE_COPY_LOCATION src_loc{};
      src_loc.pResource = s->resource.Get();
      src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src_loc.SubresourceIndex = calc_subresource(
          r.mip_level, r.base_layer + layer, 0, s->mip_levels, s->array_layers);

      if (direct_ok) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = d->resource.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Offset = slice_dst_offset;
        dst_loc.PlacedFootprint.Footprint.Format = s->copy_format;
        dst_loc.PlacedFootprint.Footprint.Width = w;
        dst_loc.PlacedFootprint.Footprint.Height = h;
        dst_loc.PlacedFootprint.Footprint.Depth = depth;
        dst_loc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(dst_row_pitch);
        cmd_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
      } else {
        const uint64_t total_rows = static_cast<uint64_t>(blocks_y) * depth;
        const uint64_t padded_bytes = aligned_pitch * total_rows;
        ComPtr<ID3D12Resource> padded = allocate_transient_buffer(
            padded_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!padded) continue;
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = padded.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Offset = 0;
        dst_loc.PlacedFootprint.Footprint.Format = s->copy_format;
        dst_loc.PlacedFootprint.Footprint.Width = w;
        dst_loc.PlacedFootprint.Footprint.Height = h;
        dst_loc.PlacedFootprint.Footprint.Depth = depth;
        dst_loc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(aligned_pitch);
        cmd_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

        transition_resource_legacy(padded.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (uint64_t row = 0; row < total_rows; ++row) {
          cmd_->CopyBufferRegion(d->resource.Get(), slice_dst_offset + row * dst_row_pitch,
                                 padded.Get(), row * aligned_pitch, dst_row_pitch);
        }
      }
    }
  }
}

void D3D12CommandListImpl::copy_texture(TextureHandle src, TextureHandle dst,
                                        const TextureCopyRegion& region) {
  const TextureEntry* s = device_.lookup_texture(src);
  const TextureEntry* d = device_.lookup_texture(dst);
  if (!s || !d || !s->resource || !d->resource) return;

  D3D12_BOX box{};
  box.left = static_cast<UINT>(region.src_x);
  box.top = static_cast<UINT>(region.src_y);
  box.front = 0;
  box.right = static_cast<UINT>(region.src_x) + region.width;
  box.bottom = static_cast<UINT>(region.src_y) + region.height;
  box.back = 1;

  for (uint32_t layer = 0; layer < std::max(region.layer_count, 1u); ++layer) {
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = s->resource.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = calc_subresource(
        region.src_mip, region.src_layer + layer, 0, s->mip_levels, s->array_layers);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = d->resource.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = calc_subresource(
        region.dst_mip, region.dst_layer + layer, 0, d->mip_levels, d->array_layers);

    cmd_->CopyTextureRegion(&dst_loc, static_cast<UINT>(region.dst_x),
                            static_cast<UINT>(region.dst_y), 0, &src_loc, &box);
  }
}

void D3D12CommandListImpl::blit(TextureHandle src, TextureHandle dst,
                                const TextureBlitRegion& region, Filter filter) {
  (void)filter;
  const TextureEntry* s = device_.lookup_texture(src);
  const TextureEntry* d = device_.lookup_texture(dst);
  if (!s || !d || !s->resource || !d->resource) return;

  const uint32_t sw = static_cast<uint32_t>(std::max(region.src_x1 - region.src_x0, 0));
  const uint32_t sh = static_cast<uint32_t>(std::max(region.src_y1 - region.src_y0, 0));
  const uint32_t dw = static_cast<uint32_t>(std::max(region.dst_x1 - region.dst_x0, 0));
  const uint32_t dh = static_cast<uint32_t>(std::max(region.dst_y1 - region.dst_y0, 0));
  const uint32_t cw = std::min(sw, dw);
  const uint32_t ch = std::min(sh, dh);
  if (cw == 0 || ch == 0) return;

  TextureCopyRegion cr{};
  cr.src_mip = region.src_mip;
  cr.src_layer = region.src_layer;
  cr.src_x = region.src_x0;
  cr.src_y = region.src_y0;
  cr.dst_mip = region.dst_mip;
  cr.dst_layer = region.dst_layer;
  cr.dst_x = region.dst_x0;
  cr.dst_y = region.dst_y0;
  cr.width = cw;
  cr.height = ch;
  cr.layer_count = region.layer_count;
  cr.is_depth = region.is_depth;
  copy_texture(src, dst, cr);
}

void D3D12CommandListImpl::clear_color_texture(TextureHandle dst, const float rgba[4]) {
  if (!rgba) return;
  const TextureEntry* te = device_.lookup_texture(dst);
  if (!te || !te->resource || !has_flag(te->usage, TextureUsage::ColorAttachment)) return;
  const uint32_t slot = device_.rtv_heap().allocate(1);
  if (slot == UINT32_MAX) return;
  D3D12_RENDER_TARGET_VIEW_DESC rtv{};
  rtv.Format = to_dxgi_format(te->format);
  rtv.ViewDimension = (te->sample_count > 1) ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                             : D3D12_RTV_DIMENSION_TEXTURE2D;
  device_.d3d_device()->CreateRenderTargetView(te->resource.Get(), &rtv,
                                               device_.rtv_heap().cpu_handle(slot));
  cmd_->ClearRenderTargetView(device_.rtv_heap().cpu_handle(slot), rgba, 0, nullptr);
  device_.rtv_heap().free(slot, 1);
}

void D3D12CommandListImpl::clear_depth_texture(TextureHandle dst, float depth) {
  const TextureEntry* te = device_.lookup_texture(dst);
  if (!te || !te->resource || !has_flag(te->usage, TextureUsage::DepthAttachment)) return;
  const uint32_t slot = device_.dsv_heap().allocate(1);
  if (slot == UINT32_MAX) return;
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = to_dxgi_dsv_format(te->format);
  dsv.ViewDimension = (te->sample_count > 1) ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                             : D3D12_DSV_DIMENSION_TEXTURE2D;
  device_.d3d_device()->CreateDepthStencilView(te->resource.Get(), &dsv,
                                               device_.dsv_heap().cpu_handle(slot));
  cmd_->ClearDepthStencilView(device_.dsv_heap().cpu_handle(slot),
                              D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
  device_.dsv_heap().free(slot, 1);
}

void D3D12CommandListImpl::resolve_depth(TextureViewHandle msaa_depth,
                                         TextureViewHandle resolved_depth,
                                         uint32_t width, uint32_t height, ResolveMode mode) {
  (void)width;
  (void)height;
  const TextureViewEntry* src_v = device_.lookup_texture_view(msaa_depth);
  const TextureViewEntry* dst_v = device_.lookup_texture_view(resolved_depth);
  if (!src_v || !dst_v) return;
  TextureEntry* src_t = device_.lookup_texture(src_v->texture);
  TextureEntry* dst_t = device_.lookup_texture(dst_v->texture);
  if (!src_t || !dst_t || !src_t->resource || !dst_t->resource) return;

  const UINT src_sub = calc_subresource(src_v->base_mip, src_v->base_layer, 0,
                                        src_t->mip_levels, src_t->array_layers);
  const UINT dst_sub = calc_subresource(dst_v->base_mip, dst_v->base_layer, 0,
                                        dst_t->mip_levels, dst_t->array_layers);
  const DXGI_FORMAT fmt = to_dxgi_srv_format(dst_t->format);
  const D3D12_RESOLVE_MODE rm = (mode == ResolveMode::Average) ? D3D12_RESOLVE_MODE_AVERAGE
                                                               : D3D12_RESOLVE_MODE_MIN;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
  if (device_.enhanced_barriers_enabled() && cmd7_ &&
      src_t->enhanced_barrier_compatible && dst_t->enhanced_barrier_compatible) {
    D3D12_TEXTURE_BARRIER tb[2]{};
    tb[0].SyncBefore = D3D12_BARRIER_SYNC_ALL;
    tb[0].SyncAfter = D3D12_BARRIER_SYNC_RESOLVE;
    tb[0].AccessBefore = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    tb[0].AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
    tb[0].LayoutBefore = src_t->current_layout;
    tb[0].LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;
    tb[0].pResource = src_t->resource.Get();
    tb[0].Subresources.IndexOrFirstMipLevel = 0xffffffff;

    tb[1].SyncBefore = (dst_t->current_layout == D3D12_BARRIER_LAYOUT_COMMON)
                           ? D3D12_BARRIER_SYNC_ALL
                           : D3D12_BARRIER_SYNC_NONE;
    tb[1].SyncAfter = D3D12_BARRIER_SYNC_RESOLVE;
    tb[1].AccessBefore = (dst_t->current_layout == D3D12_BARRIER_LAYOUT_COMMON)
                             ? D3D12_BARRIER_ACCESS_COMMON
                             : D3D12_BARRIER_ACCESS_NO_ACCESS;
    tb[1].AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
    tb[1].LayoutBefore = dst_t->current_layout;
    tb[1].LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
    tb[1].pResource = dst_t->resource.Get();
    tb[1].Subresources.IndexOrFirstMipLevel = 0xffffffff;

    D3D12_BARRIER_GROUP bg{D3D12_BARRIER_TYPE_TEXTURE, 2};
    bg.pTextureBarriers = tb;
    cmd7_->Barrier(1, &bg);

    cmd_->ResolveSubresourceRegion(dst_t->resource.Get(), dst_sub, 0, 0,
                                   src_t->resource.Get(), src_sub, nullptr, fmt, rm);

    tb[0].SyncBefore = D3D12_BARRIER_SYNC_RESOLVE;
    tb[0].SyncAfter = D3D12_BARRIER_SYNC_ALL;
    tb[0].AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
    tb[0].AccessAfter = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    tb[0].LayoutAfter = src_t->current_layout;
    tb[0].LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;

    tb[1].SyncBefore = D3D12_BARRIER_SYNC_RESOLVE;
    tb[1].SyncAfter = (dst_t->current_layout == D3D12_BARRIER_LAYOUT_COMMON)
                          ? D3D12_BARRIER_SYNC_ALL
                          : D3D12_BARRIER_SYNC_NONE;
    tb[1].AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
    tb[1].AccessAfter = (dst_t->current_layout == D3D12_BARRIER_LAYOUT_COMMON)
                            ? D3D12_BARRIER_ACCESS_COMMON
                            : D3D12_BARRIER_ACCESS_NO_ACCESS;
    tb[1].LayoutAfter = dst_t->current_layout;
    tb[1].LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
    cmd7_->Barrier(1, &bg);
    return;
  }
#endif
  const D3D12_RESOURCE_STATES prev_src = src_t->current_state;
  const D3D12_RESOURCE_STATES prev_dst = dst_t->current_state;
  transition_resource_legacy(src_t->resource.Get(), prev_src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  transition_resource_legacy(dst_t->resource.Get(), prev_dst, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  cmd_->ResolveSubresourceRegion(dst_t->resource.Get(), dst_sub, 0, 0,
                                 src_t->resource.Get(), src_sub, nullptr, fmt, rm);
  transition_resource_legacy(src_t->resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, prev_src);
  transition_resource_legacy(dst_t->resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, prev_dst);
}

void D3D12CommandListImpl::cmd_build_blas(const BlasBuildDesc& desc) {
  device_.cmd_build_blas_impl(cmd_.Get(), desc);
}

void D3D12CommandListImpl::cmd_write_blas_compacted_size(AccelStructHandle blas,
                                                         QueryPoolHandle pool,
                                                         uint32_t query_index) {
  device_.cmd_write_blas_compacted_size_impl(cmd_.Get(), blas, pool, query_index);
}

void D3D12CommandListImpl::cmd_copy_compact_blas(AccelStructHandle src, AccelStructHandle dst) {
  device_.cmd_copy_compact_blas_impl(cmd_.Get(), src, dst);
}

void D3D12CommandListImpl::cmd_build_tlas(const TlasBuildDesc& desc) {
  device_.cmd_build_tlas_impl(cmd_.Get(), desc);
}

// -----------------------------------------------------------------------------
// D3D12DeviceImpl::create_command_list, submit, submit_once & create_swapchain
// -----------------------------------------------------------------------------
namespace {

D3D12_COMMAND_LIST_TYPE d3d12_cmd_list_type(QueueKind queue, bool has_compute, bool has_copy) noexcept {
  if (queue == QueueKind::Transfer && has_copy) return D3D12_COMMAND_LIST_TYPE_COPY;
  if (queue == QueueKind::AsyncCompute && has_compute) return D3D12_COMMAND_LIST_TYPE_COMPUTE;
  return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

}  // namespace

std::unique_ptr<CommandList> D3D12DeviceImpl::create_command_list(QueueKind queue,
                                                                  std::string& error) {
  const D3D12_COMMAND_LIST_TYPE list_type =
      d3d12_cmd_list_type(queue, compute_queue_ != nullptr, copy_queue_ != nullptr);
  const QueueKind eff_kind = (list_type == D3D12_COMMAND_LIST_TYPE_COPY)      ? QueueKind::Transfer
                             : (list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) ? QueueKind::AsyncCompute
                                                                              : QueueKind::Graphics;

  ComPtr<ID3D12CommandAllocator> alloc;
  HRESULT hr = device_->CreateCommandAllocator(list_type, IID_PPV_ARGS(&alloc));
  if (FAILED(hr)) {
    error = hr_err("CreateCommandAllocator(create_command_list)", hr) +
            " (removed_reason=" + hr_err("", device_->GetDeviceRemovedReason()) + ")";
    return nullptr;
  }
  ComPtr<ID3D12GraphicsCommandList4> raw_cmd;
  hr = device_->CreateCommandList(0, list_type, alloc.Get(), nullptr, IID_PPV_ARGS(&raw_cmd));
  if (FAILED(hr)) {
    error = hr_err("CreateCommandList(create_command_list)", hr);
    return nullptr;
  }
  raw_cmd->Close();
  return std::make_unique<D3D12CommandListImpl>(*this, std::move(alloc), std::move(raw_cmd), eff_kind);
}

bool D3D12DeviceImpl::submit(QueueKind queue,
                             std::span<CommandList* const> lists,
                             std::span<const TimelineWait> waits,
                             std::span<const TimelineSignal> signals,
                             std::string& error) {
  ID3D12CommandQueue* target_queue = queue_for(queue);
  if (!target_queue) {
    error = "submit: target D3D12 command queue is null";
    return false;
  }

  std::vector<ID3D12CommandList*> raw_lists;
  raw_lists.reserve(lists.size());
  for (CommandList* cl : lists) {
    if (!cl) continue;
    auto* dcl = static_cast<D3D12CommandListImpl*>(cl);
    if (FAILED(dcl->last_close_hr())) {
      error = hr_err("ID3D12GraphicsCommandList::Close", dcl->last_close_hr());
      return false;
    }
    if (dcl->native_cmd()) raw_lists.push_back(dcl->native_cmd());
  }

  std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> wait_fences;
  wait_fences.reserve(waits.size());
  std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> signal_fences;
  signal_fences.reserve(signals.size());
  {
    std::lock_guard<std::mutex> rlock(resource_mutex_);
    for (const TimelineWait& w : waits) {
      const TimelineSemaphoreEntry* e = timeline_semaphores_.get(w.semaphore);
      if (!e || !e->fence) {
        error = "submit: invalid TimelineWait semaphore handle";
        return false;
      }
      wait_fences.emplace_back(e->fence, w.value);
    }
    for (const TimelineSignal& s : signals) {
      const TimelineSemaphoreEntry* e = timeline_semaphores_.get(s.semaphore);
      if (!e || !e->fence) {
        error = "submit: invalid TimelineSignal semaphore handle";
        return false;
      }
      signal_fences.emplace_back(e->fence, s.value);
    }
  }

  std::lock_guard<std::mutex> qlock(queue_mutex_for(queue));
  for (const auto& [fence, val] : wait_fences) {
    const HRESULT hr = target_queue->Wait(fence.Get(), val);
    if (FAILED(hr)) {
      error = hr_err("ID3D12CommandQueue::Wait", hr);
      return false;
    }
  }
  if (!raw_lists.empty()) {
    target_queue->ExecuteCommandLists(static_cast<UINT>(raw_lists.size()), raw_lists.data());
  }
  for (const auto& [fence, val] : signal_fences) {
    const HRESULT hr = target_queue->Signal(fence.Get(), val);
    if (FAILED(hr)) {
      error = hr_err("ID3D12CommandQueue::Signal", hr);
      return false;
    }
  }
  return true;
}

bool D3D12DeviceImpl::submit_once(const std::function<void(CommandList&)>& record_fn,
                                  std::string& error, bool prefer_transfer_queue) {
  const QueueKind qk =
      (prefer_transfer_queue && copy_queue_) ? QueueKind::Transfer : QueueKind::Graphics;
  const D3D12_COMMAND_LIST_TYPE list_type =
      (qk == QueueKind::Transfer) ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;

  ComPtr<ID3D12CommandAllocator> alloc;
  HRESULT hr = device_->CreateCommandAllocator(list_type, IID_PPV_ARGS(&alloc));
  if (FAILED(hr)) {
    error = hr_err("CreateCommandAllocator(submit_once)", hr);
    return false;
  }
  ComPtr<ID3D12GraphicsCommandList4> raw_cmd;
  hr = device_->CreateCommandList(0, list_type, alloc.Get(), nullptr, IID_PPV_ARGS(&raw_cmd));
  if (FAILED(hr)) {
    error = hr_err("CreateCommandList(submit_once)", hr);
    return false;
  }
  // Close immediately because begin() resets allocator + command list cleanly.
  raw_cmd->Close();

  D3D12CommandListImpl cmd(*this, alloc, raw_cmd, qk);
  cmd.begin();
  record_fn(cmd);
  cmd.end();

  ID3D12CommandList* lists[1] = {cmd.native_cmd()};
  ID3D12CommandQueue* target_queue = queue_for(qk);
  {
    std::lock_guard<std::mutex> lock(queue_mutex_for(qk));
    target_queue->ExecuteCommandLists(1, lists);
    signal_and_wait_queue(target_queue);
  }
  cmd.release_transients();
  return true;
}

std::unique_ptr<Swapchain> D3D12DeviceImpl::create_swapchain(const SwapchainDesc& desc,
                                                             std::string& error) {
  auto sc = std::make_unique<D3D12SwapchainImpl>(*this);
  if (!sc->init(desc, error)) return nullptr;
  return sc;
}

// -----------------------------------------------------------------------------
// D3D12SwapchainImpl (DXGI 1.6 Flip Model + Tearing + HDR10 / scRGB)
// -----------------------------------------------------------------------------
D3D12SwapchainImpl::D3D12SwapchainImpl(D3D12DeviceImpl& device) : device_(device) {}

D3D12SwapchainImpl::~D3D12SwapchainImpl() {
  device_.wait_idle();
  release_backbuffer_handles();
  frames_.clear();
  if (frame_latency_waitable_) {
    CloseHandle(frame_latency_waitable_);
    frame_latency_waitable_ = nullptr;
  }
  if (fence_event_) {
    CloseHandle(fence_event_);
    fence_event_ = nullptr;
  }
  frame_fence_.Reset();
  swapchain_.Reset();
}

void D3D12SwapchainImpl::release_backbuffer_handles() {
  for (TextureViewHandle vh : view_handles_) {
    device_.unregister_external_texture_view(vh);
  }
  for (TextureHandle th : texture_handles_) {
    device_.unregister_external_texture(th);
  }
  view_handles_.clear();
  texture_handles_.clear();
}

bool D3D12SwapchainImpl::create_backbuffer_handles(std::string& error) {
  release_backbuffer_handles();
  texture_handles_.reserve(buffer_count_);
  view_handles_.reserve(buffer_count_);
  for (UINT i = 0; i < buffer_count_; ++i) {
    ComPtr<ID3D12Resource> bb;
    const HRESULT hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) {
      error = hr_err("IDXGISwapChain::GetBuffer", hr);
      return false;
    }
    TextureHandle th = device_.register_external_texture(bb, dxgi_format_, width_, height_);
    TextureViewHandle vh = device_.register_external_texture_view(th, dxgi_format_);
    texture_handles_.push_back(th);
    view_handles_.push_back(vh);
  }
  return true;
}

void D3D12SwapchainImpl::configure_colorspace(bool want_hdr) {
  active_hdr_mode_ = HdrMode::Off;
  if (!swapchain_) return;

  if (want_hdr) {
    UINT support = 0;
    if (dxgi_format_ == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        SUCCEEDED(swapchain_->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
      if (SUCCEEDED(swapchain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
        active_hdr_mode_ = HdrMode::ScRgb;
        return;
      }
    }
    if (dxgi_format_ == DXGI_FORMAT_R10G10B10A2_UNORM &&
        SUCCEEDED(swapchain_->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
      if (SUCCEEDED(swapchain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))) {
        active_hdr_mode_ = HdrMode::Hdr10;
        return;
      }
    }
  }
  swapchain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
}

bool D3D12SwapchainImpl::init(const SwapchainDesc& desc, std::string& error) {
  sdl_window_ = desc.sdl_window;
#ifdef SHIFT_RHI_HAS_SDL3
  if (sdl_window_) {
    hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdl_window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  }
#endif
  if (!hwnd_) {
    error = "create_swapchain(d3d12): valid Win32 HWND from SDL3 window is required";
    return false;
  }

  width_ = desc.width;
  height_ = desc.height;
#ifdef SHIFT_RHI_HAS_SDL3
  if ((width_ == 0 || height_ == 0) && sdl_window_) {
    int sw = 0, sh = 0;
    SDL_GetWindowSizeInPixels(sdl_window_, &sw, &sh);
    width_ = static_cast<uint32_t>(std::max(sw, 1));
    height_ = static_cast<uint32_t>(std::max(sh, 1));
  }
#endif
  width_ = std::max(width_, 1u);
  height_ = std::max(height_, 1u);

  buffer_count_ = std::clamp(desc.frames_in_flight, 2u, 3u);
  allow_tearing_ = device_.allow_tearing();
  active_present_mode_ = desc.present_mode;
  if (active_present_mode_ == PresentMode::Immediate && !allow_tearing_) {
    active_present_mode_ = PresentMode::Mailbox;
  }

  dxgi_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (desc.want_hdr && device_.caps().scrgb) {
    dxgi_format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  } else if (desc.want_hdr && device_.caps().hdr10) {
    dxgi_format_ = DXGI_FORMAT_R10G10B10A2_UNORM;
  }
  format_ = from_dxgi_format(dxgi_format_);

  DXGI_SWAP_CHAIN_DESC1 scd{};
  scd.Width = width_;
  scd.Height = height_;
  scd.Format = dxgi_format_;
  scd.Stereo = FALSE;
  scd.SampleDesc.Count = 1;
  scd.SampleDesc.Quality = 0;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = buffer_count_;
  scd.Scaling = DXGI_SCALING_STRETCH;
  scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
              (allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

  ComPtr<IDXGISwapChain1> sc1;
  HRESULT hr = device_.dxgi_factory()->CreateSwapChainForHwnd(
      device_.direct_queue(), hwnd_, &scd, nullptr, nullptr, &sc1);
  if (FAILED(hr)) {
    error = hr_err("CreateSwapChainForHwnd", hr);
    return false;
  }
  device_.dxgi_factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

  hr = sc1.As(&swapchain_);
  if (FAILED(hr)) {
    error = hr_err("QueryInterface(IDXGISwapChain4)", hr);
    return false;
  }

  swapchain_->SetMaximumFrameLatency(buffer_count_);
  frame_latency_waitable_ = swapchain_->GetFrameLatencyWaitableObject();
  configure_colorspace(desc.want_hdr);

  hr = device_.d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_fence_));
  if (FAILED(hr)) {
    error = hr_err("CreateFence(swapchain)", hr);
    return false;
  }
  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fence_event_) {
    error = "CreateEventW(swapchain) failed";
    return false;
  }

  frames_.resize(buffer_count_);
  for (uint32_t i = 0; i < buffer_count_; ++i) {
    ComPtr<ID3D12CommandAllocator> alloc;
    hr = device_.d3d_device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (FAILED(hr)) {
      error = hr_err("CreateCommandAllocator(swapchain)", hr);
      return false;
    }
    ComPtr<ID3D12GraphicsCommandList4> raw_cmd;
    hr = device_.d3d_device()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&raw_cmd));
    if (FAILED(hr)) {
      error = hr_err("CreateCommandList(swapchain)", hr);
      return false;
    }
    raw_cmd->Close();
    frames_[i].cmd_list = std::make_unique<D3D12CommandListImpl>(device_, alloc, raw_cmd);
  }

  return create_backbuffer_handles(error);
}

bool D3D12SwapchainImpl::resize(uint32_t width, uint32_t height, PresentMode mode,
                                bool want_hdr, std::string& error) {
  device_.wait_idle();
  release_backbuffer_handles();

  width_ = std::max(width, 1u);
  height_ = std::max(height, 1u);
  active_present_mode_ = mode;
  if (active_present_mode_ == PresentMode::Immediate && !allow_tearing_) {
    active_present_mode_ = PresentMode::Mailbox;
  }

  dxgi_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (want_hdr && device_.caps().scrgb) {
    dxgi_format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  } else if (want_hdr && device_.caps().hdr10) {
    dxgi_format_ = DXGI_FORMAT_R10G10B10A2_UNORM;
  }
  format_ = from_dxgi_format(dxgi_format_);

  const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                     (allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
  const HRESULT hr = swapchain_->ResizeBuffers(buffer_count_, width_, height_, dxgi_format_, flags);
  if (FAILED(hr)) {
    error = hr_err("IDXGISwapChain::ResizeBuffers", hr);
    return false;
  }
  configure_colorspace(want_hdr);
  return create_backbuffer_handles(error);
}

bool D3D12SwapchainImpl::acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                                            bool& out_of_date, std::string& error) {
  out_of_date = false;
  if (!swapchain_ || width_ == 0 || height_ == 0) {
    out_of_date = true;
    return true;
  }

  if (frame_latency_waitable_) {
    const DWORD ms = (timeout_ns == UINT64_MAX)
                         ? INFINITE
                         : static_cast<DWORD>(std::min<uint64_t>(timeout_ns / 1000000ull, 10000ull));
    WaitForSingleObject(frame_latency_waitable_, ms);
  }

  FrameSlot& slot = frames_[current_slot_];
  if (slot.fence_value > 0 && frame_fence_->GetCompletedValue() < slot.fence_value) {
    if (SUCCEEDED(frame_fence_->SetEventOnCompletion(slot.fence_value, fence_event_))) {
      WaitForSingleObject(fence_event_, INFINITE);
    }
  }
  slot.cmd_list->reset_for_recording();

  const UINT bb_index = swapchain_->GetCurrentBackBufferIndex();
  if (bb_index >= texture_handles_.size()) {
    error = "acquire_next_frame: backbuffer index out of bounds";
    return false;
  }

  out_frame.image_index = bb_index;
  out_frame.frame_slot = current_slot_;
  out_frame.texture = texture_handles_[bb_index];
  out_frame.view = view_handles_[bb_index];
  out_frame.format = format_;
  out_frame.width = width_;
  out_frame.height = height_;
  out_frame.suboptimal = false;
  return true;
}

CommandList& D3D12SwapchainImpl::frame_command_list(uint32_t frame_slot) {
  return *frames_[frame_slot % frames_.size()].cmd_list;
}

bool D3D12SwapchainImpl::submit_and_present(const SwapchainFrame& frame,
                                            bool& out_of_date, std::string& error) {
  out_of_date = false;
  FrameSlot& slot = frames_[frame.frame_slot % frames_.size()];
  ID3D12CommandList* lists[1] = {slot.cmd_list->native_cmd()};

  {
    std::lock_guard<std::mutex> lock(device_.queue_mutex());
    device_.direct_queue()->ExecuteCommandLists(1, lists);
  }

  UINT sync_interval = 1;
  UINT present_flags = 0;
  if (active_present_mode_ == PresentMode::Immediate && allow_tearing_) {
    sync_interval = 0;
    present_flags = DXGI_PRESENT_ALLOW_TEARING;
  } else if (active_present_mode_ == PresentMode::Mailbox) {
    sync_interval = 0;
  }

  const HRESULT hr = swapchain_->Present(sync_interval, present_flags);
  {
    std::lock_guard<std::mutex> lock(device_.queue_mutex());
    slot.fence_value = next_fence_value_++;
    device_.direct_queue()->Signal(frame_fence_.Get(), slot.fence_value);
  }
  current_slot_ = (current_slot_ + 1u) % static_cast<uint32_t>(frames_.size());

  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    error = hr_err("IDXGISwapChain::Present", hr);
    return false;
  }
  if (hr == DXGI_STATUS_OCCLUDED) {
    return true;
  }
  if (FAILED(hr)) {
    out_of_date = true;
    error = hr_err("IDXGISwapChain::Present", hr);
    return false;
  }
  return true;
}

}  // namespace shift::rhi::d3d12
