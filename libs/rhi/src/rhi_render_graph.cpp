#include "rhi/render_graph.h"

#include <algorithm>
#include <queue>

namespace shift::rhi {

// -----------------------------------------------------------------------------
// RenderGraphBuilder
// -----------------------------------------------------------------------------
void RenderGraphBuilder::read_texture(RGTextureHandle handle, ResourceState state) {
  if (!handle.valid() || handle.index >= graph_.textures_.size()) return;
  graph_.passes_[pass_index_].texture_accesses.push_back({handle.index, state, false});
}

void RenderGraphBuilder::write_texture(RGTextureHandle handle, ResourceState state) {
  if (!handle.valid() || handle.index >= graph_.textures_.size()) return;
  graph_.passes_[pass_index_].texture_accesses.push_back({handle.index, state, true});
}

void RenderGraphBuilder::read_buffer(RGBufferHandle handle, ResourceState state) {
  if (!handle.valid() || handle.index >= graph_.buffers_.size()) return;
  graph_.passes_[pass_index_].buffer_accesses.push_back({handle.index, state, false});
}

void RenderGraphBuilder::write_buffer(RGBufferHandle handle, ResourceState state) {
  if (!handle.valid() || handle.index >= graph_.buffers_.size()) return;
  graph_.passes_[pass_index_].buffer_accesses.push_back({handle.index, state, true});
}

void RenderGraphBuilder::has_side_effects() {
  graph_.passes_[pass_index_].side_effects = true;
}

// -----------------------------------------------------------------------------
// RenderGraphContext
// -----------------------------------------------------------------------------
TextureHandle RenderGraphContext::texture(RGTextureHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= graph_.textures_.size()) return {};
  return graph_.textures_[handle.index].handle;
}

TextureViewHandle RenderGraphContext::texture_view(RGTextureHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= graph_.textures_.size()) return {};
  return graph_.textures_[handle.index].view;
}

BufferHandle RenderGraphContext::buffer(RGBufferHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= graph_.buffers_.size()) return {};
  return graph_.buffers_[handle.index].handle;
}

const RenderExtent& RenderGraphContext::render_extent() const noexcept {
  return graph_.render_extent_;
}

// -----------------------------------------------------------------------------
// RenderGraph lifecycle & resource declaration
// -----------------------------------------------------------------------------
RenderGraph::RenderGraph(Device& device) : device_(device) {}

RenderGraph::~RenderGraph() {
  release_pool();
}

void RenderGraph::reset() {
  textures_.clear();
  buffers_.clear();
  passes_.clear();
  exec_order_.clear();
  pass_deps_.clear();
  pre_pass_barriers_.clear();
  post_pass_barriers_.clear();
  epilogue_barriers_ = {};
  batches_.clear();
  stats_ = {};
  compiled_ = false;
  for (uint32_t q = 0; q < 3; ++q) cmd_list_cursor_[q] = 0;
  for (auto& pt : texture_pool_) {
    pt.last_used_exec_step = UINT32_MAX;
    pt.last_owner_pass = UINT32_MAX;
    pt.used_this_frame = false;
  }
  for (auto& pb : buffer_pool_) {
    pb.last_used_exec_step = UINT32_MAX;
    pb.last_owner_pass = UINT32_MAX;
    pb.used_this_frame = false;
  }
}

void RenderGraph::release_pool() {
  for (uint32_t q = 0; q < 3; ++q) {
    if (last_submitted_timelines_[q] > 0 && timeline_sems_[q]) {
      device_.wait_timeline_semaphore(timeline_sems_[q], last_submitted_timelines_[q], UINT64_MAX);
    }
  }
  for (uint32_t q = 0; q < 3; ++q) {
    cmd_lists_[q].clear();
    cmd_list_cursor_[q] = 0;
  }
  for (auto& pt : texture_pool_) {
    if (pt.view) device_.destroy_texture_view(pt.view);
    if (pt.texture) device_.destroy_texture(pt.texture);
  }
  texture_pool_.clear();
  for (auto& pb : buffer_pool_) {
    if (pb.buffer) device_.destroy_buffer(pb.buffer);
  }
  buffer_pool_.clear();
  for (uint32_t q = 0; q < 3; ++q) {
    if (timeline_sems_[q]) {
      device_.destroy_timeline_semaphore(timeline_sems_[q]);
      timeline_sems_[q] = {};
    }
    timeline_counters_[q] = 0;
    last_submitted_timelines_[q] = 0;
  }
}

RGTextureHandle RenderGraph::create_texture(const char* name, const TextureDesc& desc) {
  TextureNode node{};
  node.name = name ? name : "rg_tex";
  node.imported = false;
  node.desc = desc;
  node.format = desc.format;
  node.initial_state = ResourceState::Undefined;
  node.final_state = ResourceState::Undefined;
  const uint32_t idx = static_cast<uint32_t>(textures_.size());
  textures_.push_back(std::move(node));
  compiled_ = false;
  return RGTextureHandle{idx};
}

RGBufferHandle RenderGraph::create_buffer(const char* name, const BufferDesc& desc) {
  BufferNode node{};
  node.name = name ? name : "rg_buf";
  node.imported = false;
  node.desc = desc;
  node.initial_state = ResourceState::Common;
  node.final_state = ResourceState::Undefined;
  const uint32_t idx = static_cast<uint32_t>(buffers_.size());
  buffers_.push_back(std::move(node));
  compiled_ = false;
  return RGBufferHandle{idx};
}

RGTextureHandle RenderGraph::import_texture(const char* name,
                                            TextureHandle texture,
                                            TextureViewHandle view,
                                            ResourceState initial_state,
                                            ResourceState final_state,
                                            Format format) {
  TextureNode node{};
  node.name = name ? name : "rg_imported_tex";
  node.imported = true;
  node.handle = texture;
  node.view = view;
  node.initial_state = initial_state;
  node.final_state = final_state;
  node.format = format;
  const uint32_t idx = static_cast<uint32_t>(textures_.size());
  textures_.push_back(std::move(node));
  compiled_ = false;
  return RGTextureHandle{idx};
}

RGBufferHandle RenderGraph::import_buffer(const char* name,
                                          BufferHandle buffer,
                                          ResourceState initial_state,
                                          ResourceState final_state) {
  BufferNode node{};
  node.name = name ? name : "rg_imported_buf";
  node.imported = true;
  node.handle = buffer;
  node.initial_state = initial_state;
  node.final_state = final_state;
  const uint32_t idx = static_cast<uint32_t>(buffers_.size());
  buffers_.push_back(std::move(node));
  compiled_ = false;
  return RGBufferHandle{idx};
}

uint32_t RenderGraph::add_pass(const char* name, QueueKind queue, SetupFn setup_fn,
                               ExecuteFn execute_fn) {
  const uint32_t idx = static_cast<uint32_t>(passes_.size());
  PassNode pass{};
  pass.name = name ? name : "rg_pass";
  pass.queue = queue;
  pass.execute = std::move(execute_fn);
  passes_.push_back(std::move(pass));
  if (setup_fn) {
    RenderGraphBuilder builder(*this, idx);
    setup_fn(builder);
  }
  compiled_ = false;
  return idx;
}

bool RenderGraph::texture_desc_compatible(const TextureDesc& a, const TextureDesc& b) noexcept {
  return a.type == b.type && a.format == b.format && a.width == b.width &&
         a.height == b.height && a.depth == b.depth && a.mip_levels == b.mip_levels &&
         a.array_layers == b.array_layers && a.sample_count == b.sample_count &&
         a.usage == b.usage && a.memory == b.memory;
}

bool RenderGraph::buffer_desc_compatible(const BufferDesc& a, const BufferDesc& b) noexcept {
  return a.size == b.size && a.usage == b.usage && a.memory == b.memory &&
         a.persistently_mapped == b.persistently_mapped;
}

// -----------------------------------------------------------------------------
// Compilation: DAG edges, dead-pass culling, topological sort, aliasing, barriers
// -----------------------------------------------------------------------------
bool RenderGraph::compile(std::string& error) {
  error.clear();
  compiled_ = false;
  exec_order_.clear();
  pass_deps_.clear();
  pre_pass_barriers_.clear();
  post_pass_barriers_.clear();
  epilogue_barriers_ = {};
  batches_.clear();
  stats_ = {};

  const uint32_t num_passes = static_cast<uint32_t>(passes_.size());
  stats_.total_passes = num_passes;
  if (num_passes == 0) {
    compiled_ = true;
    return true;
  }

  // 1. Build RAW / WAW / WAR dependency edges in declaration order.
  pass_deps_.assign(num_passes, {});
  std::vector<std::vector<uint32_t>> pass_succ(num_passes);
  auto add_edge = [&](uint32_t producer, uint32_t consumer) {
    if (producer == consumer) return;
    auto& deps = pass_deps_[consumer];
    if (std::find(deps.begin(), deps.end(), producer) == deps.end()) {
      deps.push_back(producer);
      pass_succ[producer].push_back(consumer);
    }
  };

  std::vector<uint32_t> tex_last_writer(textures_.size(), UINT32_MAX);
  std::vector<std::vector<uint32_t>> tex_readers_since_write(textures_.size());
  std::vector<uint32_t> buf_last_writer(buffers_.size(), UINT32_MAX);
  std::vector<std::vector<uint32_t>> buf_readers_since_write(buffers_.size());

  for (uint32_t p = 0; p < num_passes; ++p) {
    passes_[p].culled = false;
    for (const ResourceAccess& a : passes_[p].texture_accesses) {
      if (a.is_write) {
        if (tex_last_writer[a.resource_index] != UINT32_MAX) {
          add_edge(tex_last_writer[a.resource_index], p);  // WAW
        }
        for (uint32_t r : tex_readers_since_write[a.resource_index]) {
          add_edge(r, p);  // WAR
        }
        tex_last_writer[a.resource_index] = p;
        tex_readers_since_write[a.resource_index].clear();
      } else {
        if (tex_last_writer[a.resource_index] != UINT32_MAX) {
          add_edge(tex_last_writer[a.resource_index], p);  // RAW
        }
        tex_readers_since_write[a.resource_index].push_back(p);
      }
    }
    for (const ResourceAccess& a : passes_[p].buffer_accesses) {
      if (a.is_write) {
        if (buf_last_writer[a.resource_index] != UINT32_MAX) {
          add_edge(buf_last_writer[a.resource_index], p);  // WAW
        }
        for (uint32_t r : buf_readers_since_write[a.resource_index]) {
          add_edge(r, p);  // WAR
        }
        buf_last_writer[a.resource_index] = p;
        buf_readers_since_write[a.resource_index].clear();
      } else {
        if (buf_last_writer[a.resource_index] != UINT32_MAX) {
          add_edge(buf_last_writer[a.resource_index], p);  // RAW
        }
        buf_readers_since_write[a.resource_index].push_back(p);
      }
    }
  }

  // 2. Dead-pass culling: keep passes with side effects or writes to imported resources,
  //    plus all transitive dependencies feeding them.
  std::vector<bool> alive(num_passes, !culling_enabled_);
  if (culling_enabled_) {
    std::queue<uint32_t> q;
    for (uint32_t p = 0; p < num_passes; ++p) {
      bool root = passes_[p].side_effects;
      if (!root) {
        for (const ResourceAccess& a : passes_[p].texture_accesses) {
          if (a.is_write && textures_[a.resource_index].imported) {
            root = true;
            break;
          }
        }
      }
      if (!root) {
        for (const ResourceAccess& a : passes_[p].buffer_accesses) {
          if (a.is_write && buffers_[a.resource_index].imported) {
            root = true;
            break;
          }
        }
      }
      if (root) {
        alive[p] = true;
        q.push(p);
      }
    }
    while (!q.empty()) {
      const uint32_t cur = q.front();
      q.pop();
      for (uint32_t dep : pass_deps_[cur]) {
        if (!alive[dep]) {
          alive[dep] = true;
          q.push(dep);
        }
      }
    }
    for (uint32_t p = 0; p < num_passes; ++p) {
      passes_[p].culled = !alive[p];
      if (!alive[p]) ++stats_.culled_passes;
    }
  }

  // 3. Topological sort (Kahn's algorithm using min-heap to preserve declaration order).
  std::vector<uint32_t> indegree(num_passes, 0);
  for (uint32_t p = 0; p < num_passes; ++p) {
    if (!alive[p]) continue;
    for (uint32_t dep : pass_deps_[p]) {
      if (alive[dep]) ++indegree[p];
    }
  }
  std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
  for (uint32_t p = 0; p < num_passes; ++p) {
    if (alive[p] && indegree[p] == 0) ready.push(p);
  }
  while (!ready.empty()) {
    const uint32_t p = ready.top();
    ready.pop();
    exec_order_.push_back(p);
    for (uint32_t next : pass_succ[p]) {
      if (!alive[next]) continue;
      if (--indegree[next] == 0) ready.push(next);
    }
  }

  const uint32_t expected_alive = num_passes - stats_.culled_passes;
  if (exec_order_.size() != expected_alive) {
    error = "RenderGraph::compile: cycle detected in pass dependency graph";
    return false;
  }
  stats_.executed_passes = static_cast<uint32_t>(exec_order_.size());

  // 4. Compute transient resource lifetimes in execution-step space.
  for (auto& t : textures_) {
    t.first_exec_step = UINT32_MAX;
    t.last_exec_step = 0;
    t.physical_slot = UINT32_MAX;
  }
  for (auto& b : buffers_) {
    b.first_exec_step = UINT32_MAX;
    b.last_exec_step = 0;
    b.physical_slot = UINT32_MAX;
  }
  for (uint32_t step = 0; step < exec_order_.size(); ++step) {
    const PassNode& pn = passes_[exec_order_[step]];
    for (const ResourceAccess& a : pn.texture_accesses) {
      TextureNode& tn = textures_[a.resource_index];
      tn.first_exec_step = std::min(tn.first_exec_step, step);
      tn.last_exec_step = std::max(tn.last_exec_step, step);
    }
    for (const ResourceAccess& a : pn.buffer_accesses) {
      BufferNode& bn = buffers_[a.resource_index];
      bn.first_exec_step = std::min(bn.first_exec_step, step);
      bn.last_exec_step = std::max(bn.last_exec_step, step);
    }
  }

  // Reset per-frame pool tracking so repeated compile() calls without reset() stay clean.
  for (auto& pt : texture_pool_) {
    pt.last_used_exec_step = UINT32_MAX;
    pt.last_owner_pass = UINT32_MAX;
    pt.used_this_frame = false;
  }
  for (auto& pb : buffer_pool_) {
    pb.last_used_exec_step = UINT32_MAX;
    pb.last_owner_pass = UINT32_MAX;
    pb.used_this_frame = false;
  }

  // 5. Materialize & alias transient textures/buffers across disjoint lifetimes.
  if (!materialize_transient_resources(error)) return false;

  // 6. Plan batched intra-frame resource barriers per execution step.
  pre_pass_barriers_.assign(exec_order_.size(), {});
  post_pass_barriers_.assign(exec_order_.size(), {});
  std::vector<ResourceState> tex_state(textures_.size(), ResourceState::Undefined);
  std::vector<ResourceState> buf_state(buffers_.size(), ResourceState::Common);
  for (size_t i = 0; i < textures_.size(); ++i) tex_state[i] = textures_[i].initial_state;
  for (size_t i = 0; i < buffers_.size(); ++i) buf_state[i] = buffers_[i].initial_state;

  // Track physical-slot state for aliased transient resources across frames & aliases.
  std::vector<ResourceState> pool_tex_state(texture_pool_.size(), ResourceState::Undefined);
  for (size_t i = 0; i < texture_pool_.size(); ++i) {
    pool_tex_state[i] = texture_pool_[i].current_state;
  }
  std::vector<ResourceState> pool_buf_state(buffer_pool_.size(), ResourceState::Common);

  // Precompute next queue using each texture (or aliased physical slot) after a given
  // execution step so Graphics passes release graphics-only layouts to Common before a
  // non-Graphics queue (AsyncCompute/Transfer) accesses them.
  auto is_graphics_only_layout = [](ResourceState st) noexcept {
    return st == ResourceState::RenderTarget || st == ResourceState::DepthWrite ||
           st == ResourceState::DepthRead || st == ResourceState::Present;
  };
  auto next_texture_queue = [&](uint32_t tex_idx, uint32_t after_step) -> QueueKind {
    const uint32_t phys = textures_[tex_idx].imported ? UINT32_MAX : textures_[tex_idx].physical_slot;
    for (uint32_t s = after_step + 1u; s < exec_order_.size(); ++s) {
      const PassNode& np = passes_[exec_order_[s]];
      for (const ResourceAccess& a : np.texture_accesses) {
        if (a.resource_index == tex_idx) return np.queue;
        if (phys != UINT32_MAX && !textures_[a.resource_index].imported &&
            textures_[a.resource_index].physical_slot == phys) {
          return np.queue;
        }
      }
    }
    return QueueKind::Graphics;
  };

  for (uint32_t step = 0; step < exec_order_.size(); ++step) {
    const PassNode& pn = passes_[exec_order_[step]];
    PlannedBarrier& pb = pre_pass_barriers_[step];
    PlannedBarrier& post = post_pass_barriers_[step];

    for (const ResourceAccess& a : pn.texture_accesses) {
      const TextureNode& tn = textures_[a.resource_index];
      if (!tn.handle) continue;
      ResourceState& cur =
          (!tn.imported && tn.physical_slot < pool_tex_state.size())
              ? pool_tex_state[tn.physical_slot]
              : tex_state[a.resource_index];
      const bool needs_uav_barrier =
          (cur == ResourceState::StorageReadWrite && a.state == ResourceState::StorageReadWrite);
      if (cur != a.state || needs_uav_barrier) {
        TextureBarrier tb{};
        tb.texture = tn.handle;
        tb.before = cur;
        tb.after = a.state;
        tb.mip_count = tn.imported ? 1u : std::max(tn.desc.mip_levels, 1u);
        tb.layer_count = tn.imported ? 1u : std::max(tn.desc.array_layers, 1u);
        tb.is_depth = format_is_depth(tn.format);
        pb.texture_barriers.push_back(tb);
        cur = a.state;
      }

      if (pn.queue == QueueKind::Graphics && is_graphics_only_layout(cur) &&
          next_texture_queue(a.resource_index, step) != QueueKind::Graphics) {
        TextureBarrier rel{};
        rel.texture = tn.handle;
        rel.before = cur;
        rel.after = ResourceState::Common;
        rel.mip_count = tn.imported ? 1u : std::max(tn.desc.mip_levels, 1u);
        rel.layer_count = tn.imported ? 1u : std::max(tn.desc.array_layers, 1u);
        rel.is_depth = format_is_depth(tn.format);
        post.texture_barriers.push_back(rel);
        cur = ResourceState::Common;
      }
    }

    for (const ResourceAccess& a : pn.buffer_accesses) {
      const BufferNode& bn = buffers_[a.resource_index];
      if (!bn.handle) continue;
      ResourceState& cur =
          (!bn.imported && bn.physical_slot < pool_buf_state.size())
              ? pool_buf_state[bn.physical_slot]
              : buf_state[a.resource_index];
      const bool needs_uav_barrier =
          (cur == ResourceState::StorageReadWrite && a.state == ResourceState::StorageReadWrite);
      if (cur != a.state || needs_uav_barrier) {
        BufferBarrier bb{};
        bb.buffer = bn.handle;
        bb.before = cur;
        bb.after = a.state;
        pb.buffer_barriers.push_back(bb);
        cur = a.state;
      }
    }
  }

  // Persist end-of-frame physical pooled texture states across frames; release any
  // graphics-only pooled texture to Common so next frame can use it on any queue safely.
  for (size_t i = 0; i < texture_pool_.size(); ++i) {
    if (texture_pool_[i].used_this_frame) {
      if (is_graphics_only_layout(pool_tex_state[i]) &&
          texture_pool_[i].last_used_exec_step < post_pass_barriers_.size() &&
          passes_[exec_order_[texture_pool_[i].last_used_exec_step]].queue == QueueKind::Graphics) {
        TextureBarrier rel{};
        rel.texture = texture_pool_[i].texture;
        rel.before = pool_tex_state[i];
        rel.after = ResourceState::Common;
        rel.mip_count = std::max(texture_pool_[i].desc.mip_levels, 1u);
        rel.layer_count = std::max(texture_pool_[i].desc.array_layers, 1u);
        rel.is_depth = format_is_depth(texture_pool_[i].desc.format);
        post_pass_barriers_[texture_pool_[i].last_used_exec_step].texture_barriers.push_back(rel);
        pool_tex_state[i] = ResourceState::Common;
      }
      texture_pool_[i].current_state = pool_tex_state[i];
    }
  }

  // Final-state epilogue barriers for imported resources (e.g., Present, DepthRead,
  // CopySrc, HostRead): attach each transition to the post_pass_barriers_ of the
  // last pass that accessed that resource if that pass's queue can legally perform it.
  for (size_t i = 0; i < textures_.size(); ++i) {
    const TextureNode& tn = textures_[i];
    if (!tn.imported || !tn.handle || tn.final_state == ResourceState::Undefined) continue;
    if (tex_state[i] != tn.final_state) {
      TextureBarrier tb{};
      tb.texture = tn.handle;
      tb.before = tex_state[i];
      tb.after = tn.final_state;
      tb.is_depth = format_is_depth(tn.format);
      const bool can_attach_to_last =
          tn.first_exec_step != UINT32_MAX && tn.last_exec_step < post_pass_barriers_.size() &&
          (!is_graphics_only_layout(tn.final_state) ||
           passes_[exec_order_[tn.last_exec_step]].queue == QueueKind::Graphics);
      if (can_attach_to_last) {
        post_pass_barriers_[tn.last_exec_step].texture_barriers.push_back(tb);
      } else if (!is_graphics_only_layout(tn.final_state) ||
                 (!exec_order_.empty() && passes_[exec_order_.back()].queue == QueueKind::Graphics)) {
        epilogue_barriers_.texture_barriers.push_back(tb);
      }
      tex_state[i] = tn.final_state;
    }
  }
  for (size_t i = 0; i < buffers_.size(); ++i) {
    const BufferNode& bn = buffers_[i];
    if (!bn.imported || !bn.handle || bn.final_state == ResourceState::Undefined) continue;
    if (buf_state[i] != bn.final_state) {
      BufferBarrier bb{};
      bb.buffer = bn.handle;
      bb.before = buf_state[i];
      bb.after = bn.final_state;
      if (bn.first_exec_step != UINT32_MAX && bn.last_exec_step < post_pass_barriers_.size()) {
        post_pass_barriers_[bn.last_exec_step].buffer_barriers.push_back(bb);
      } else {
        epilogue_barriers_.buffer_barriers.push_back(bb);
      }
      buf_state[i] = bn.final_state;
    }
  }

  // 7. Group execution steps into QueueBatches with automatic cross-queue timeline syncs.
  std::vector<uint32_t> pass_to_batch(num_passes, UINT32_MAX);
  for (uint32_t step = 0; step < exec_order_.size(); ++step) {
    const uint32_t p = exec_order_[step];
    const QueueKind qk = passes_[p].queue;

    // Collect cross-queue predecessor batches that this pass depends on.
    std::vector<uint32_t> xqueue_waits;
    for (uint32_t dep : pass_deps_[p]) {
      const uint32_t dep_batch = pass_to_batch[dep];
      if (dep_batch == UINT32_MAX) continue;
      if (batches_[dep_batch].queue != qk) {
        if (std::find(xqueue_waits.begin(), xqueue_waits.end(), dep_batch) == xqueue_waits.end()) {
          xqueue_waits.push_back(dep_batch);
        }
      }
    }

    // Coalesce into the current batch if same queue AND no new cross-queue waits
    // (a wait applies to the whole submission, so a pass introducing a new wait
    // starts a fresh batch).
    bool can_append = !batches_.empty() && batches_.back().queue == qk && xqueue_waits.empty();
    if (!can_append) {
      QueueBatch batch{};
      batch.queue = qk;
      batch.wait_batches = std::move(xqueue_waits);
      for (uint32_t wb : batch.wait_batches) {
        batches_[wb].signals_timeline = true;
        ++stats_.cross_queue_syncs;
      }
      batches_.push_back(std::move(batch));
    }
    const uint32_t cur_batch = static_cast<uint32_t>(batches_.size()) - 1u;
    batches_[cur_batch].exec_steps.push_back(step);
    pass_to_batch[p] = cur_batch;
  }
  // Signal the final batch of every queue that has work so wait_for_last_execution()
  // synchronizes all concurrent queues without violating per-queue timeline monotonicity.
  uint32_t last_batch_per_queue[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  for (uint32_t bi = 0; bi < static_cast<uint32_t>(batches_.size()); ++bi) {
    last_batch_per_queue[static_cast<size_t>(batches_[bi].queue)] = bi;
  }
  for (uint32_t q = 0; q < 3; ++q) {
    if (last_batch_per_queue[q] != UINT32_MAX) {
      batches_[last_batch_per_queue[q]].signals_timeline = true;
    }
  }
  stats_.queue_submissions = static_cast<uint32_t>(batches_.size());

  compiled_ = true;
  return true;
}

bool RenderGraph::materialize_transient_resources(std::string& error) {
  // Sort transient texture indices by first_exec_step for greedy interval coloring.
  std::vector<uint32_t> transient_tex;
  for (uint32_t i = 0; i < textures_.size(); ++i) {
    if (!textures_[i].imported && textures_[i].first_exec_step != UINT32_MAX) {
      transient_tex.push_back(i);
    }
  }
  stats_.transient_textures_declared = static_cast<uint32_t>(transient_tex.size());
  std::sort(transient_tex.begin(), transient_tex.end(), [&](uint32_t a, uint32_t b) {
    return textures_[a].first_exec_step < textures_[b].first_exec_step;
  });

  for (uint32_t idx : transient_tex) {
    TextureNode& tn = textures_[idx];
    uint32_t chosen = UINT32_MAX;
    for (uint32_t slot = 0; slot < texture_pool_.size(); ++slot) {
      PooledTexture& pt = texture_pool_[slot];
      if (!texture_desc_compatible(pt.desc, tn.desc)) continue;
      if (pt.last_used_exec_step == UINT32_MAX || pt.last_used_exec_step < tn.first_exec_step) {
        chosen = slot;
        if (pt.used_this_frame) ++stats_.aliased_textures;
        break;
      }
    }
    if (chosen == UINT32_MAX) {
      PooledTexture pt{};
      pt.desc = tn.desc;
      pt.texture = device_.create_texture(tn.desc, error);
      if (!pt.texture) return false;
      TextureViewDesc vd{};
      vd.texture = pt.texture;
      vd.type = tn.desc.type;
      vd.format = tn.desc.format;
      vd.mip_count = std::max(tn.desc.mip_levels, 1u);
      vd.layer_count = std::max(tn.desc.array_layers, 1u);
      pt.view = device_.create_texture_view(vd, error);
      if (!pt.view) {
        device_.destroy_texture(pt.texture);
        return false;
      }
      chosen = static_cast<uint32_t>(texture_pool_.size());
      texture_pool_.push_back(std::move(pt));
    }
    if (texture_pool_[chosen].used_this_frame && texture_pool_[chosen].last_owner_pass != UINT32_MAX &&
        tn.first_exec_step < exec_order_.size()) {
      const uint32_t prev_owner = texture_pool_[chosen].last_owner_pass;
      const uint32_t next_owner = exec_order_[tn.first_exec_step];
      if (prev_owner != next_owner) {
        auto& deps = pass_deps_[next_owner];
        if (std::find(deps.begin(), deps.end(), prev_owner) == deps.end()) {
          deps.push_back(prev_owner);
        }
      }
    }
    texture_pool_[chosen].last_used_exec_step = tn.last_exec_step;
    texture_pool_[chosen].last_owner_pass =
        (tn.last_exec_step < exec_order_.size()) ? exec_order_[tn.last_exec_step] : UINT32_MAX;
    texture_pool_[chosen].used_this_frame = true;
    tn.physical_slot = chosen;
    tn.handle = texture_pool_[chosen].texture;
    tn.view = texture_pool_[chosen].view;
  }

  for (const auto& pt : texture_pool_) {
    if (pt.used_this_frame) ++stats_.physical_textures_allocated;
  }

  // Same interval-coloring aliasing for transient buffers.
  std::vector<uint32_t> transient_buf;
  for (uint32_t i = 0; i < buffers_.size(); ++i) {
    if (!buffers_[i].imported && buffers_[i].first_exec_step != UINT32_MAX) {
      transient_buf.push_back(i);
    }
  }
  stats_.transient_buffers_declared = static_cast<uint32_t>(transient_buf.size());
  std::sort(transient_buf.begin(), transient_buf.end(), [&](uint32_t a, uint32_t b) {
    return buffers_[a].first_exec_step < buffers_[b].first_exec_step;
  });

  for (uint32_t idx : transient_buf) {
    BufferNode& bn = buffers_[idx];
    uint32_t chosen = UINT32_MAX;
    for (uint32_t slot = 0; slot < buffer_pool_.size(); ++slot) {
      PooledBuffer& pb = buffer_pool_[slot];
      if (!buffer_desc_compatible(pb.desc, bn.desc)) continue;
      if (pb.last_used_exec_step == UINT32_MAX || pb.last_used_exec_step < bn.first_exec_step) {
        chosen = slot;
        if (pb.used_this_frame) ++stats_.aliased_buffers;
        break;
      }
    }
    if (chosen == UINT32_MAX) {
      PooledBuffer pb{};
      pb.desc = bn.desc;
      pb.buffer = device_.create_buffer(bn.desc, error);
      if (!pb.buffer) return false;
      chosen = static_cast<uint32_t>(buffer_pool_.size());
      buffer_pool_.push_back(std::move(pb));
    }
    if (buffer_pool_[chosen].used_this_frame && buffer_pool_[chosen].last_owner_pass != UINT32_MAX &&
        bn.first_exec_step < exec_order_.size()) {
      const uint32_t prev_owner = buffer_pool_[chosen].last_owner_pass;
      const uint32_t next_owner = exec_order_[bn.first_exec_step];
      if (prev_owner != next_owner) {
        auto& deps = pass_deps_[next_owner];
        if (std::find(deps.begin(), deps.end(), prev_owner) == deps.end()) {
          deps.push_back(prev_owner);
        }
      }
    }
    buffer_pool_[chosen].last_used_exec_step = bn.last_exec_step;
    buffer_pool_[chosen].last_owner_pass =
        (bn.last_exec_step < exec_order_.size()) ? exec_order_[bn.last_exec_step] : UINT32_MAX;
    buffer_pool_[chosen].used_this_frame = true;
    bn.physical_slot = chosen;
    bn.handle = buffer_pool_[chosen].buffer;
  }

  for (const auto& pb : buffer_pool_) {
    if (pb.used_this_frame) ++stats_.physical_buffers_allocated;
  }
  return true;
}

bool RenderGraph::ensure_sync_objects(std::string& error) {
  for (uint32_t q = 0; q < 3; ++q) {
    if (!timeline_sems_[q]) {
      timeline_sems_[q] = device_.create_timeline_semaphore(0, error);
      if (!timeline_sems_[q]) return false;
    }
  }
  return true;
}

// -----------------------------------------------------------------------------
// Execution: Multi-Queue Timeline Submission & Single-Queue CommandList
// -----------------------------------------------------------------------------
bool RenderGraph::execute(std::string& error,
                          bool wait_for_completion,
                          uint64_t* out_final_timeline_value) {
  if (!compiled_) {
    if (!compile(error)) return false;
  }
  if (batches_.empty()) {
    if (out_final_timeline_value) *out_final_timeline_value = 0;
    return true;
  }
  if (!ensure_sync_objects(error)) return false;

  // Ensure prior frame command lists across all queues have finished before resetting them.
  wait_for_last_execution(UINT64_MAX);
  for (uint32_t q = 0; q < 3; ++q) cmd_list_cursor_[q] = 0;

  // Assign strictly monotonic per-queue timeline signal values (one timeline semaphore per
  // queue guarantees Vulkan timeline monotonicity even when queues finish out-of-order).
  for (QueueBatch& b : batches_) {
    const size_t q_idx = static_cast<size_t>(b.queue);
    if (b.signals_timeline) {
      b.signal_value = ++timeline_counters_[q_idx];
    } else {
      b.signal_value = 0;
    }
  }

  const RenderGraphContext ctx(*this);

  for (size_t bi = 0; bi < batches_.size(); ++bi) {
    const QueueBatch& batch = batches_[bi];
    const size_t q_idx = static_cast<size_t>(batch.queue);
    const uint32_t slot = cmd_list_cursor_[q_idx]++;
    if (slot >= cmd_lists_[q_idx].size()) {
      auto cl = device_.create_command_list(batch.queue, error);
      if (!cl) {
        error = "batch " + std::to_string(bi) + " (" + queue_kind_name(batch.queue) + "): " + error;
        return false;
      }
      cmd_lists_[q_idx].push_back(std::move(cl));
    }
    CommandList& cmd = *cmd_lists_[q_idx][slot];
    cmd.begin();

    for (uint32_t step : batch.exec_steps) {
      const PlannedBarrier& pb = pre_pass_barriers_[step];
      if (!pb.texture_barriers.empty() || !pb.buffer_barriers.empty()) {
        BarrierDesc bd{};
        bd.textures = pb.texture_barriers;
        bd.buffers = pb.buffer_barriers;
        cmd.barrier(bd);
      }
      const PassNode& pn = passes_[exec_order_[step]];
      if (pn.execute) {
        pn.execute(cmd, ctx);
      }
      const PlannedBarrier& post = post_pass_barriers_[step];
      if (!post.texture_barriers.empty() || !post.buffer_barriers.empty()) {
        BarrierDesc bd{};
        bd.textures = post.texture_barriers;
        bd.buffers = post.buffer_barriers;
        cmd.barrier(bd);
      }
    }

    // Emit epilogue final-state transitions at the end of the last batch.
    if (bi + 1 == batches_.size() &&
        (!epilogue_barriers_.texture_barriers.empty() ||
         !epilogue_barriers_.buffer_barriers.empty())) {
      BarrierDesc bd{};
      bd.textures = epilogue_barriers_.texture_barriers;
      bd.buffers = epilogue_barriers_.buffer_barriers;
      cmd.barrier(bd);
    }

    cmd.end();

    std::vector<TimelineWait> waits;
    waits.reserve(batch.wait_batches.size());
    for (uint32_t wb : batch.wait_batches) {
      if (batches_[wb].signal_value > 0) {
        const size_t dep_q = static_cast<size_t>(batches_[wb].queue);
        waits.push_back(TimelineWait{timeline_sems_[dep_q], batches_[wb].signal_value,
                                     PipelineStage::AllCommands});
      }
    }

    std::vector<TimelineSignal> signals;
    if (batch.signal_value > 0) {
      signals.push_back(TimelineSignal{timeline_sems_[q_idx], batch.signal_value,
                                       PipelineStage::AllCommands});
    }

    CommandList* cl_ptr = &cmd;
    if (!device_.submit(batch.queue, std::span(&cl_ptr, 1), waits, signals, error)) {
      error = "batch " + std::to_string(bi) + " (" + queue_kind_name(batch.queue) + "): " + error;
      return false;
    }
    if (batch.signal_value > 0) {
      last_submitted_timelines_[q_idx] = batch.signal_value;
    }
  }

  uint64_t max_timeline = 0;
  for (uint32_t q = 0; q < 3; ++q) {
    max_timeline = std::max(max_timeline, last_submitted_timelines_[q]);
  }
  if (out_final_timeline_value) *out_final_timeline_value = max_timeline;

  if (wait_for_completion) {
    if (!wait_for_last_execution(UINT64_MAX)) {
      error = "RenderGraph::execute: wait_timeline_semaphore failed";
      return false;
    }
  }
  return true;
}

bool RenderGraph::execute_into(CommandList& cmd, std::string& error) {
  if (!compiled_) {
    if (!compile(error)) return false;
  }
  if (batches_.size() > 1) {
    error = "RenderGraph::execute_into: compiled graph spans multiple queues; use execute()";
    return false;
  }
  const RenderGraphContext ctx(*this);
  for (uint32_t step = 0; step < exec_order_.size(); ++step) {
    const PlannedBarrier& pb = pre_pass_barriers_[step];
    if (!pb.texture_barriers.empty() || !pb.buffer_barriers.empty()) {
      BarrierDesc bd{};
      bd.textures = pb.texture_barriers;
      bd.buffers = pb.buffer_barriers;
      cmd.barrier(bd);
    }
    const PassNode& pn = passes_[exec_order_[step]];
    if (pn.execute) {
      pn.execute(cmd, ctx);
    }
    const PlannedBarrier& post = post_pass_barriers_[step];
    if (!post.texture_barriers.empty() || !post.buffer_barriers.empty()) {
      BarrierDesc bd{};
      bd.textures = post.texture_barriers;
      bd.buffers = post.buffer_barriers;
      cmd.barrier(bd);
    }
  }
  if (!epilogue_barriers_.texture_barriers.empty() ||
      !epilogue_barriers_.buffer_barriers.empty()) {
    BarrierDesc bd{};
    bd.textures = epilogue_barriers_.texture_barriers;
    bd.buffers = epilogue_barriers_.buffer_barriers;
    cmd.barrier(bd);
  }
  return true;
}

bool RenderGraph::wait_for_last_execution(uint64_t timeout_ns) {
  bool all_ok = true;
  for (uint32_t q = 0; q < 3; ++q) {
    if (last_submitted_timelines_[q] == 0 || !timeline_sems_[q]) continue;
    if (!device_.wait_timeline_semaphore(timeline_sems_[q], last_submitted_timelines_[q],
                                         timeout_ns)) {
      all_ok = false;
    }
  }
  return all_ok;
}

}  // namespace shift::rhi
