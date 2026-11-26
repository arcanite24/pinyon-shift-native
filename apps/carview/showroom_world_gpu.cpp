#include "showroom_internal.h"

#include "world_gpu_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace carview {

namespace {

// Per-prop-instance index spans recorded off-thread in stage_world() from cached RAM.
// Allows smashing a single fence panel / sign / pole inside a 64m or 256m merged draw
// without hiding neighboring intact props or failing on wide/sloped cell AABBs.
std::mutex g_smash_hide_mutex;
std::unordered_map<const Showroom*, gpuworld::SmashHideTracker> g_smash_hide;

}  // namespace

bool Showroom::begin_world_texture(const WorldTextureData& t, VkBuffer texels, VkDeviceSize offset, uint32_t levels,
                                   uint32_t skip, VkCommandBuffer cmd, Renderer::Image* made, std::string& error) {
  GpuTexture image;
  if (made && made->image) {
    image = *made;
    *made = {};
  } else if (!create_texture_image(t.texture, t.srgb, levels, image, error, skip)) {
    r_.destroy_image(image);
    return false;
  }
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!create_world_single_set(image.view, set, error)) {
    r_.destroy_image(image);
    return false;
  }
  record_texture_copy(cmd, texels, offset, t.texture, levels, image.image, skip);
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    world_textures_[t.record] = image;
    world_sets_[t.record] = set;
    world_lru_.insert(t.record, image.bytes, world_frame_);
    if (gpu_world_) gpu_world_->add_texture(t.record, image.view);
  }
  return true;
}


bool Showroom::allocate_world_set(VkDescriptorSet& out, std::string& error) {
  if (!free_world_sets_.empty()) {
    out = free_world_sets_.back();
    free_world_sets_.pop_back();
    return true;
  }
  if (world_pool_left_ == 0) {
    constexpr uint32_t kSets = 256;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 8};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = kSets;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    VkDescriptorPool created = VK_NULL_HANDLE;
    VK_TRY(vkCreateDescriptorPool(r_.device_, &pool, nullptr, &created), "world descriptor pool");
    world_pools_.push_back(created);
    world_pool_left_ = kSets;
  }
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = world_pools_.back();
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &world_material_layout_;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &out), "world material set");
  --world_pool_left_;
  ++world_sets_allocated_;
  return true;
}

bool Showroom::create_world_single_set(VkImageView view, VkDescriptorSet& out, std::string& error) {
  if (gpu_textures_.size() < 2) {
    error = "no built-in white/black texture";
    return false;
  }
  if (!allocate_world_set(out, error)) return false;
  const VkImageView white = gpu_textures_[0].view;
  const VkImageView black = gpu_textures_[1].view;
  const VkImageView flat_n = flat_normal_tex_.view ? flat_normal_tex_.view : white;
  const VkImageView views[8] = {view, white, white, black, flat_n, white, white, white};
  VkDescriptorImageInfo images[8];
  VkWriteDescriptorSet writes[8];
  for (uint32_t b = 0; b < 8; ++b) {
    images[b] = {sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[b].dstSet = out;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[b].pImageInfo = &images[b];
  }
  vkUpdateDescriptorSets(r_.device_, 8, writes, 0, nullptr);
  return true;
}

bool Showroom::upload_world_textures(const std::vector<std::shared_ptr<const WorldTextureData>>& textures,
                                     std::string& error) {
  if (!world_white_set_) {
    if (gpu_textures_.empty()) {
      error = "no built-in white texture";
      return false;
    }
    if (!create_world_single_set(gpu_textures_[0].view, world_white_set_, error)) return false;
  }
  for (const auto& t : textures) {
    if (!t) continue;
    {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      if (world_textures_.count(t->record)) continue;
    }
    GpuTexture image;
    std::string why;
    if (!upload_texture(&t->texture, t->srgb, image, why)) {
      r_.destroy_image(image);
      notes_.push_back("world texture " + t->texture.name + ": " + why);
      continue;  // drawn flat
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!create_world_single_set(image.view, set, error)) {
      r_.destroy_image(image);
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      world_textures_[t->record] = image;
      world_sets_[t->record] = set;
      world_lru_.insert(t->record, image.bytes, world_frame_);
      if (gpu_world_) gpu_world_->add_texture(t->record, image.view);
    }
  }
  return true;
}

size_t Showroom::world_texture_count() const {
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  return world_textures_.size();
}

bool Showroom::has_world_texture(uint32_t record) const {
  if (record == kNoWorldTexture) return false;
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  return world_sets_.count(record) != 0;
}

bool Showroom::ensure_world_texture(const std::shared_ptr<const WorldTextureData>& tex, std::string& error) {
  if (!tex) return false;
  std::vector<std::shared_ptr<const WorldTextureData>> one{tex};
  return upload_world_textures(one, error);
}

bool Showroom::replace_world_texture(uint32_t record, const bake::Texture& texture, bool srgb, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    if (!world_textures_.count(record)) return false;
  }
  GpuTexture image;
  if (!upload_texture(&texture, srgb, image, error)) {
    r_.destroy_image(image);
    return false;
  }
  vkDeviceWaitIdle(r_.device_);
  GpuTexture old;
  std::vector<std::pair<VkDescriptorSet, uint32_t>> bindings;  // set, binding
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    auto it = world_textures_.find(record);
    if (it == world_textures_.end()) {
      r_.destroy_image(image);
      return false;
    }
    old = it->second;
    it->second = image;
    if (auto s = world_sets_.find(record); s != world_sets_.end()) bindings.emplace_back(s->second, 0);
    for (const auto& [key, set] : world_layer_sets_)
      for (uint32_t b = 0; b < 8; ++b)
        if (key[b] == record) bindings.emplace_back(set, b);
    world_lru_.insert(record, image.bytes, world_frame_);
  }
  const VkDescriptorImageInfo info{sampler_, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  std::vector<VkWriteDescriptorSet> writes;
  for (const auto& [set, b] : bindings) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = b;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    writes.push_back(w);
  }
  if (!writes.empty()) vkUpdateDescriptorSets(r_.device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
  r_.destroy_image(old);
  return true;
}

void Showroom::set_world_texture_budget(const WorldTextureBudget& budget) {
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    world_budget_explicit_ = true;
    world_lru_.set_budget(budget.max_bytes, budget.max_count);
  }
  if (r_.device_) evict_world_textures();
}

WorldTextureBudget Showroom::world_texture_budget() const {
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  return world_lru_.budget();
}

Showroom::WorldTextureStats Showroom::world_texture_stats() const {
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  WorldTextureStats s;
  s.resident_count = world_textures_.size();
  s.retired_pending = retired_.size() + retired_sets_.size();
  s.descriptor_sets_allocated = world_sets_allocated_;
  s.descriptor_sets_free = free_world_sets_.size();
  s.descriptor_pools = world_pools_.size();
  s.resident_bytes = world_lru_.total_bytes();
  s.uploads = world_lru_.uploads();
  s.evictions = world_lru_.evictions();
  s.reuploads = world_lru_.reuploads();
  return s;
}

bool Showroom::reserve_world_texture(uint32_t record) {
  if (record == kNoWorldTexture) return false;
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  if (!world_textures_.count(record)) return false;
  ++prepare_pins_[record];
  return true;
}

void Showroom::pin_prepare_records(const std::vector<uint32_t>& records) {
  if (records.empty()) return;
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  for (uint32_t record : records) {
    if (record != kNoWorldTexture) ++prepare_pins_[record];
  }
}

void Showroom::release_prepare_pins(const std::vector<uint32_t>& records) {
  if (records.empty()) return;
  std::lock_guard<std::mutex> lock(world_textures_mutex_);
  for (uint32_t record : records) {
    auto it = prepare_pins_.find(record);
    if (it == prepare_pins_.end()) continue;
    if (it->second <= 1) prepare_pins_.erase(it);
    else --it->second;
  }
}

void Showroom::evict_world_textures() {
  std::vector<GpuTexture> evicted_images;
  std::vector<VkDescriptorSet> evicted_sets;
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    const auto b = world_lru_.budget();
    if (b.max_bytes == 0 && b.max_count == 0) return;
    if ((b.max_bytes == 0 || world_lru_.total_bytes() <= b.max_bytes) &&
        (b.max_count == 0 || world_lru_.count() <= b.max_count))
      return;
    const uint64_t epoch = world_lru_.begin_pin_epoch();
    auto pin_draw = [&](const WorldGeometry::Draw& d) {
      if (d.texture != kNoWorldTexture) world_lru_.pin(d.texture, epoch);
      if (d.texture_b != kNoWorldTexture) world_lru_.pin(d.texture_b, epoch);
      if (d.splat != kNoWorldTexture) world_lru_.pin(d.splat, epoch);
      if (d.light_map != kNoWorldTexture) world_lru_.pin(d.light_map, epoch);
      if (d.normal_map != kNoWorldTexture) world_lru_.pin(d.normal_map, epoch);
      if (d.ao_map != kNoWorldTexture) world_lru_.pin(d.ao_map, epoch);
      if (d.layer_c != kNoWorldTexture) world_lru_.pin(d.layer_c, epoch);
      if (d.spec_map != kNoWorldTexture) world_lru_.pin(d.spec_map, epoch);
    };
    for (const auto& d : world_draws_) pin_draw(d);
    for (const auto& t : world_trees_)
      if (t.texture != kNoWorldTexture) world_lru_.pin(t.texture, epoch);
    for (uint32_t record : active_gpu_table_records_)
      if (record != kNoWorldTexture) world_lru_.pin(record, epoch);
    if (fading_on_)
      for (const auto& d : fading_.draws) pin_draw(d);
    if (pending_world_) {
      for (const auto& d : pending_world_->staged.draws) pin_draw(d);
      for (const auto& t : pending_world_->staged.trees)
        if (t.texture != kNoWorldTexture) world_lru_.pin(t.texture, epoch);
      for (const auto& t : pending_world_->staged.textures)
        if (t) world_lru_.pin(t->record, epoch);
    }
    {
      std::lock_guard<std::mutex> qlock(deferred_uploads_mutex_);
      for (const auto& t : deferred_world_textures_)
        if (t) world_lru_.pin(t->record, epoch);
    }
    for (const auto& [record, count] : prepare_pins_)
      if (count > 0) world_lru_.pin(record, epoch);
    std::vector<uint32_t> victims;
    world_lru_.evict_unpinned(epoch, victims);
    if (victims.empty()) return;
    evicted_images.reserve(victims.size());
    std::unordered_set<uint32_t> victim_set;
    victim_set.reserve(victims.size() * 2 + 8);
    for (uint32_t record : victims) {
      victim_set.insert(record);
      if (gpu_world_) gpu_world_->remove_texture(record);
      if (auto it = world_textures_.find(record); it != world_textures_.end()) {
        evicted_images.push_back(it->second);
        world_textures_.erase(it);
      }
      if (auto it = world_sets_.find(record); it != world_sets_.end()) {
        evicted_sets.push_back(it->second);
        world_sets_.erase(it);
      }
    }
    if (!world_layer_sets_.empty()) {
      for (auto it = world_layer_sets_.begin(); it != world_layer_sets_.end();) {
        const auto& k = it->first;
        bool match = false;
        for (uint32_t r_id : k) {
          if (r_id != kNoWorldTexture && victim_set.count(r_id)) {
            match = true;
            break;
          }
        }
        if (match) {
          evicted_sets.push_back(it->second);
          it = world_layer_sets_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (!evicted_images.empty()) {
    retire([this, imgs = std::move(evicted_images)]() mutable {
      for (auto& img : imgs) r_.destroy_image(img);
    });
  }
  if (!evicted_sets.empty()) {
    retired_sets_.push_back({r_.frames_submitted(), std::move(evicted_sets)});
  }
}

VkDescriptorSet Showroom::world_layer_set(const std::array<uint32_t, 8>& records) {
  const auto it = world_layer_sets_.find(records);
  if (it != world_layer_sets_.end()) return it->second;
  if (gpu_textures_.size() < 2) return VK_NULL_HANDLE;
  const VkImageView white = gpu_textures_[0].view;
  const VkImageView black = gpu_textures_[1].view;
  const VkImageView flat_n = flat_normal_tex_.view ? flat_normal_tex_.view : white;
  const VkImageView defaults[8] = {white, white, white, black, flat_n, white, white, white};
  VkDescriptorImageInfo images[8];
  VkWriteDescriptorSet writes[8];
  for (uint32_t b = 0; b < 8; ++b) {
    VkImageView view = defaults[b];
    if (records[b] != kNoWorldTexture) {
      const auto t = world_textures_.find(records[b]);
      if (t == world_textures_.end()) {
        if (b < 3) return VK_NULL_HANDLE;
      } else {
        view = t->second.view;
      }
    }
    images[b] = {sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[b].dstBinding = b;
    writes[b].pImageInfo = &images[b];
  }
  VkDescriptorSet set = VK_NULL_HANDLE;
  std::string error;
  if (!allocate_world_set(set, error)) return VK_NULL_HANDLE;
  for (auto& w : writes) w.dstSet = set;
  vkUpdateDescriptorSets(r_.device_, 8, writes, 0, nullptr);
  world_layer_sets_[records] = set;
  return set;
}

VkDescriptorSet Showroom::world_set(uint32_t record) const {
  const auto it = world_sets_.find(record);
  return it == world_sets_.end() ? VK_NULL_HANDLE : it->second;
}


bool Showroom::arena_on() const { return gpu_world_ && gpu_world_->ready(); }

void Showroom::world_bind_buffers(VkCommandBuffer cmd, bool fading) {
  VkDeviceSize zero = 0;
  if (arena_on() && (!fading || fading_.arena)) {
    VkBuffer vb = gpu_world_->vertex_buffer(), ib = gpu_world_->index_buffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    return;
  }
  const Renderer::Buffer& v = fading ? fading_.vertices : world_vertices_;
  const Renderer::Buffer& i = fading ? fading_.indices : world_indices_;
  vkCmdBindVertexBuffers(cmd, 0, 1, &v.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, i.buffer, 0, VK_INDEX_TYPE_UINT32);
}

// A draw's model matrix: the scenery's transform with the draw's origin shift (a resident draw's positions are
// relative to the gather that made it) folded into the translation.
void Showroom::world_draw_model(const WorldGeometry::Draw& d, const float transform[16], float out[16]) const {
  std::memcpy(out, transform, 16 * sizeof(float));
  if (d.shift[0] == 0.0f && d.shift[1] == 0.0f) return;
  for (int r = 0; r < 4; ++r) out[12 + r] += transform[r] * d.shift[0] + transform[8 + r] * d.shift[1];
}

bool Showroom::arena_lookup_draw(uint64_t key, uint32_t& out_index_base) const {
  if (!arena_on()) return false;
  if (!arena_use_rt_resident_ && arena_lru_.vertex_capacity() > 0) {
    const auto* e = arena_lru_.find(key);
    if (!e) return false;
    out_index_base = e->index_base;
    return true;
  }
  const auto* rd = gpu_world_->resident().find(key);
  if (!rd) return false;
  out_index_base = rd->index_base;
  return true;
}

bool Showroom::arena_contains_draw(uint64_t key) const {
  if (!arena_on()) return false;
  if (!arena_use_rt_resident_ && arena_lru_.vertex_capacity() > 0) return arena_lru_.contains(key);
  return gpu_world_->resident().contains(key);
}

void Showroom::arena_mark_draw_valid(uint64_t key) {
  if (!arena_on()) return;
  if (!arena_use_rt_resident_ && arena_lru_.vertex_capacity() > 0) {
    arena_lru_.mark_valid(key);
    return;
  }
  gpu_world_->resident().mark_valid(key);
}

void Showroom::arena_release_draw(uint64_t key, uint64_t tag) {
  if (!arena_on()) return;
  if (!arena_use_rt_resident_ && arena_lru_.vertex_capacity() > 0) {
    arena_lru_.release(key, tag);
    return;
  }
  gpu_world_->resident().release(key, tag);
}

void Showroom::world_release_draws(std::vector<WorldGeometry::Draw>& draws) {
  if (!arena_on()) return;
  const uint64_t tag = r_.frames_submitted();
  for (auto& d : draws) {
    if (!d.resident) continue;
    arena_release_draw(d.key, tag);
    d.resident = false;
  }
  // Prune per-instance prop span metadata for draws no longer resident in the arena.
  std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
  if (auto it = g_smash_hide.find(this); it != g_smash_hide.end()) {
    auto& by_key = it->second.by_draw_key;
    for (auto kit = by_key.begin(); kit != by_key.end();) {
      if (!arena_contains_draw(kit->first)) kit = by_key.erase(kit);
      else ++kit;
    }
  }
}

// Gives each draw of a pending world its place in the arenas: found by key (a regather that left it as it was
// uploads nothing) or placed via O(1) LRU eviction + O(log F) coalescing free-block allocation without ever
// wiping the cached world or stalling on vkDeviceWaitIdle() during live driving.
bool Showroom::place_world_arena(StagedWorld& st, std::vector<WorldGpu::Copy>& vertex_copies,
                                 std::vector<WorldGpu::Copy>& index_copies, std::string& error,
                                 bool* deferred_busy, bool allow_blocking_reset) {
  if (deferred_busy) *deferred_busy = false;
  st.uploaded_keys.clear();
  auto& res = gpu_world_->resident();
  const uint32_t cap_v = res.vertex_capacity();
  const uint32_t cap_i = res.index_capacity();
  const bool want_rt = (r_.active_rt_mode() != rt::RtMode::kOff);

  if (arena_lru_.vertex_capacity() != cap_v || arena_lru_.index_capacity() != cap_i) {
    arena_lru_.reset(cap_v, cap_i);
    arena_use_rt_resident_ = want_rt;
  } else if (want_rt != arena_use_rt_resident_) {
    if (world_draws_.empty() && arena_lru_.count() == 0 && res.count() == 0) {
      arena_use_rt_resident_ = want_rt;
    } else {
      // Live RT mode toggle: cleanly switch backing resident tracker once.
      r_.wait_idle();
      drop_fading_world(true);
      for (auto& d : world_draws_) d.resident = false;
      world_draws_.clear();
      world_trees_.clear();
      active_gpu_table_records_.clear();
      gpu_table_missing_textures_.clear();
      if (gpu_world_) {
        std::string clear_err;
        (void)gpu_world_->set_table({}, clear_err);
      }
      {
        std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
        if (auto it = g_smash_hide.find(this); it != g_smash_hide.end()) it->second.by_draw_key.clear();
      }
      release_retired();
      arena_lru_.reset(cap_v, cap_i);
      res.reset(cap_v, cap_i);
      arena_use_rt_resident_ = want_rt;
    }
  }

  std::vector<uint32_t> staged_first(st.draws.size());
  for (size_t i = 0; i < st.draws.size(); ++i) staged_first[i] = st.draws[i].first_index;
  const uint64_t generation = ++world_generation_;
  size_t fail_idx = 0;

  // Release any completed swap-fade draws early so their space is immediately reclaimable by O(1) LRU.
  if (fading_on_) drop_fading_world(false);

  if (!arena_use_rt_resident_) {
    std::vector<const WorldArenaLru::Entry*> placed(st.draws.size(), nullptr);
    auto attempt_lru = [&]() {
      vertex_copies.clear();
      index_copies.clear();
      st.uploaded_keys.clear();
      vertex_copies.reserve(st.draws.size());
      index_copies.reserve(st.draws.size());
      for (size_t i = 0; i < st.draws.size(); ++i) {
        auto& d = st.draws[i];
        placed[i] = nullptr;
        if (d.vertex_count == 0 || d.index_count == 0) continue;
        const WorldArenaLru::Entry* rd = arena_lru_.acquire(d.key, generation);
        const bool known = rd != nullptr;
        if (!rd)
          rd = arena_lru_.place(d.key, d.vertex_count, d.index_count, st.origin_x, st.origin_z, generation,
                                r_.frames_done());
        if (!rd) {
          fail_idx = i;
          st.uploaded_keys.clear();
          for (size_t k = 0; k < i; ++k)
            if (placed[k]) arena_lru_.release(st.draws[k].key, 0);
          return false;
        }
        placed[i] = rd;
        if (!known) {
          st.uploaded_keys.insert(d.key);
          vertex_copies.push_back({VkDeviceSize(d.vertex_begin) * sizeof(WorldVertex),
                                   VkDeviceSize(rd->vertex_base) * sizeof(WorldVertex),
                                   VkDeviceSize(d.vertex_count) * sizeof(WorldVertex)});
          index_copies.push_back({VkDeviceSize(staged_first[i]) * sizeof(uint32_t),
                                  VkDeviceSize(rd->index_base) * sizeof(uint32_t),
                                  VkDeviceSize(d.index_count) * sizeof(uint32_t)});
        }
      }
      return true;
    };

    if (!attempt_lru()) {
      // If unreferenced draws are only pinned by an in-flight GPU frame (free_tag > frames_done)
      // and the live + incoming draws fit within arena capacity, defer placement to the next frame
      // instead of stalling on vkDeviceWaitIdle() and wiping cached world VRAM.
      if (!allow_blocking_reset && deferred_busy && arena_lru_.has_inflight_unreferenced(r_.frames_done())) {
        uint64_t live_v = 0, live_i = 0, need_v = 0, need_i = 0;
        std::unordered_set<uint64_t> seen_live;
        seen_live.reserve(world_draws_.size() * 2 + 16);
        for (const auto& d : world_draws_) {
          if (!d.resident || !seen_live.insert(d.key).second) continue;
          if (const auto* e = arena_lru_.find(d.key)) {
            live_v += e->vertex_count;
            live_i += e->index_count;
          }
        }
        std::unordered_set<uint64_t> seen_new;
        seen_new.reserve(st.draws.size() * 2 + 16);
        for (const auto& d : st.draws) {
          if (d.vertex_count == 0 || d.index_count == 0) continue;
          if (seen_live.count(d.key) || !seen_new.insert(d.key).second) continue;
          need_v += d.vertex_count;
          need_i += d.index_count;
        }
        if (live_v + need_v <= uint64_t(cap_v) && live_i + need_i <= uint64_t(cap_i)) {
          st.uploaded_keys.clear();
          *deferred_busy = true;
          return true;
        }
      }

      // Last-resort fallback only when arena capacity is genuinely smaller than live + incoming draws.
      r_.wait_idle();
      drop_fading_world(true);
      for (auto& d : world_draws_) d.resident = false;
      world_draws_.clear();
      world_trees_.clear();
      active_gpu_table_records_.clear();
      gpu_table_missing_textures_.clear();
      if (gpu_world_) {
        std::string clear_err;
        (void)gpu_world_->set_table({}, clear_err);
      }
      {
        std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
        if (auto it = g_smash_hide.find(this); it != g_smash_hide.end()) it->second.by_draw_key.clear();
      }
      release_retired();
      arena_lru_.reset(cap_v, cap_i);
      if (!attempt_lru()) {
        // Never abort the game if a dense gather (e.g. Horizon Festival on an unusually small
        // arena override) exceeds capacity: place nearest draws first and skip only overflow tail.
        arena_lru_.reset(cap_v, cap_i);
        vertex_copies.clear();
        index_copies.clear();
        st.uploaded_keys.clear();
        std::fill(placed.begin(), placed.end(), nullptr);
        std::vector<size_t> order(st.draws.size());
        for (size_t i = 0; i < st.draws.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
          const auto& da = st.draws[a];
          const auto& db = st.draws[b];
          const float ax = 0.5f * (da.box_min[0] + da.box_max[0]);
          const float az = 0.5f * (da.box_min[2] + da.box_max[2]);
          const float bx = 0.5f * (db.box_min[0] + db.box_max[0]);
          const float bz = 0.5f * (db.box_min[2] + db.box_max[2]);
          return (ax * ax + az * az) < (bx * bx + bz * bz);
        });
        size_t skipped = 0;
        for (size_t idx : order) {
          auto& d = st.draws[idx];
          if (d.vertex_count == 0 || d.index_count == 0) continue;
          const WorldArenaLru::Entry* rd = arena_lru_.acquire(d.key, generation);
          const bool known = rd != nullptr;
          if (!rd)
            rd = arena_lru_.place(d.key, d.vertex_count, d.index_count, st.origin_x, st.origin_z, generation,
                                  r_.frames_done());
          if (!rd) {
            d.vertex_count = 0;
            d.index_count = 0;
            d.resident = false;
            ++skipped;
            continue;
          }
          placed[idx] = rd;
          if (!known) {
            st.uploaded_keys.insert(d.key);
            vertex_copies.push_back({VkDeviceSize(d.vertex_begin) * sizeof(WorldVertex),
                                     VkDeviceSize(rd->vertex_base) * sizeof(WorldVertex),
                                     VkDeviceSize(d.vertex_count) * sizeof(WorldVertex)});
            index_copies.push_back({VkDeviceSize(staged_first[idx]) * sizeof(uint32_t),
                                    VkDeviceSize(rd->index_base) * sizeof(uint32_t),
                                    VkDeviceSize(d.index_count) * sizeof(uint32_t)});
          }
        }
        if (skipped > 0) {
          std::fprintf(stderr, "carview: note: world arena tight (%llu/%u verts); skipped %zu distant draws\n",
                       (unsigned long long)arena_lru_.vertex_used(), arena_lru_.vertex_capacity(), skipped);
        }
      }
    }

    for (size_t i = 0; i < st.draws.size(); ++i) {
      auto& d = st.draws[i];
      const WorldArenaLru::Entry* rd = placed[i];
      if (!rd) continue;
      d.first_index = rd->index_base;
      d.vertex_offset = int32_t(rd->vertex_base);
      d.shift[0] = float(rd->origin_x - st.origin_x);
      d.shift[1] = float(rd->origin_z - st.origin_z);
      for (int k = 0; k < 3; k += 2) {
        d.box_min[k] += d.shift[k / 2];
        d.box_max[k] += d.shift[k / 2];
      }
      d.resident = true;
    }
    return true;
  }

  // RT-mode path: keeps WorldGpu::resident() populated for RtScene BLAS lookups without premature thrash wipes.
  std::vector<const gpuworld::ResidentDraw*> placed(st.draws.size(), nullptr);
  auto attempt_rt = [&]() {
    vertex_copies.clear();
    index_copies.clear();
    st.uploaded_keys.clear();
    vertex_copies.reserve(st.draws.size());
    index_copies.reserve(st.draws.size());
    for (size_t i = 0; i < st.draws.size(); ++i) {
      auto& d = st.draws[i];
      placed[i] = nullptr;
      if (d.vertex_count == 0 || d.index_count == 0) continue;
      const gpuworld::ResidentDraw* rd = res.acquire(d.key, generation);
      const bool known = rd != nullptr;
      if (!rd)
        rd = res.place(d.key, d.vertex_count, d.index_count, st.origin_x, st.origin_z, generation, r_.frames_done());
      if (!rd) {
        fail_idx = i;
        st.uploaded_keys.clear();
        for (size_t k = 0; k < i; ++k)
          if (placed[k]) res.release(st.draws[k].key, 0);
        return false;
      }
      placed[i] = rd;
      if (!known) {
        st.uploaded_keys.insert(d.key);
        vertex_copies.push_back({VkDeviceSize(d.vertex_begin) * sizeof(WorldVertex),
                                 VkDeviceSize(rd->vertex_base) * sizeof(WorldVertex),
                                 VkDeviceSize(d.vertex_count) * sizeof(WorldVertex)});
        index_copies.push_back({VkDeviceSize(staged_first[i]) * sizeof(uint32_t),
                                VkDeviceSize(rd->index_base) * sizeof(uint32_t),
                                VkDeviceSize(d.index_count) * sizeof(uint32_t)});
      }
    }
    return true;
  };
  if (!attempt_rt()) {
    if (!allow_blocking_reset && deferred_busy && res.count() > res.referenced() &&
        r_.frames_submitted() > r_.frames_done()) {
      st.uploaded_keys.clear();
      *deferred_busy = true;
      return true;
    }
    r_.wait_idle();
    drop_fading_world(true);
    for (auto& d : world_draws_) d.resident = false;
    world_draws_.clear();
    world_trees_.clear();
    active_gpu_table_records_.clear();
    gpu_table_missing_textures_.clear();
    if (gpu_world_) {
      std::string clear_err;
      (void)gpu_world_->set_table({}, clear_err);
    }
    {
      std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
      if (auto it = g_smash_hide.find(this); it != g_smash_hide.end()) it->second.by_draw_key.clear();
    }
    release_retired();
    res.reset(cap_v, cap_i);
    if (!attempt_rt()) {
      res.reset(cap_v, cap_i);
      vertex_copies.clear();
      index_copies.clear();
      st.uploaded_keys.clear();
      std::fill(placed.begin(), placed.end(), nullptr);
      std::vector<size_t> order(st.draws.size());
      for (size_t i = 0; i < st.draws.size(); ++i) order[i] = i;
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& da = st.draws[a];
        const auto& db = st.draws[b];
        const float ax = 0.5f * (da.box_min[0] + da.box_max[0]);
        const float az = 0.5f * (da.box_min[2] + da.box_max[2]);
        const float bx = 0.5f * (db.box_min[0] + db.box_max[0]);
        const float bz = 0.5f * (db.box_min[2] + db.box_max[2]);
        return (ax * ax + az * az) < (bx * bx + bz * bz);
      });
      size_t skipped = 0;
      for (size_t idx : order) {
        auto& d = st.draws[idx];
        if (d.vertex_count == 0 || d.index_count == 0) continue;
        const gpuworld::ResidentDraw* rd = res.acquire(d.key, generation);
        const bool known = rd != nullptr;
        if (!rd)
          rd = res.place(d.key, d.vertex_count, d.index_count, st.origin_x, st.origin_z, generation, r_.frames_done());
        if (!rd) {
          d.vertex_count = 0;
          d.index_count = 0;
          d.resident = false;
          ++skipped;
          continue;
        }
        placed[idx] = rd;
        if (!known) {
          st.uploaded_keys.insert(d.key);
          vertex_copies.push_back({VkDeviceSize(d.vertex_begin) * sizeof(WorldVertex),
                                   VkDeviceSize(rd->vertex_base) * sizeof(WorldVertex),
                                   VkDeviceSize(d.vertex_count) * sizeof(WorldVertex)});
          index_copies.push_back({VkDeviceSize(staged_first[idx]) * sizeof(uint32_t),
                                  VkDeviceSize(rd->index_base) * sizeof(uint32_t),
                                  VkDeviceSize(d.index_count) * sizeof(uint32_t)});
        }
      }
      if (skipped > 0) {
        std::fprintf(stderr, "carview: note: world arena tight (%llu/%u verts); skipped %zu distant draws\n",
                     (unsigned long long)res.vertex_used(), res.vertex_capacity(), skipped);
      }
    }
  }
  for (size_t i = 0; i < st.draws.size(); ++i) {
    auto& d = st.draws[i];
    const gpuworld::ResidentDraw* rd = placed[i];
    if (!rd) continue;
    d.first_index = rd->index_base;
    d.vertex_offset = int32_t(rd->vertex_base);
    d.shift[0] = float(rd->origin_x - st.origin_x);
    d.shift[1] = float(rd->origin_z - st.origin_z);
    for (int k = 0; k < 3; k += 2) {
      d.box_min[k] += d.shift[k / 2];
      d.box_max[k] += d.shift[k / 2];
    }
    d.resident = true;
  }
  return true;
}

void Showroom::install_world_prop_spans(StagedWorld& st) {
  if (!arena_on()) return;
  std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
  auto& state = g_smash_hide[this];
  state.install_spans(
      st.prop_spans, st.uploaded_keys,
      [this](uint64_t key) { return arena_contains_draw(key); },
      [this](uint64_t key, uint32_t& base) { return arena_lookup_draw(key, base); },
      [this](uint32_t first, uint32_t count) { gpu_world_->queue_index_zero(first, count); });
  st.uploaded_keys.clear();
}

// The table the culling pass reads: one row per resident opaque or cut-out draw,
// plus template rows for hardware-instanced tree sprites.
bool Showroom::is_smashable_prop_draw(uint32_t flags, uint8_t decal, float ex, float ey, float ez) {
  // Never hide sky dome (2), terrain layers (4 vertex-blend, 8 splat, 16384 layer-C),
  // roads (1048576), or projected world-space road decals (fade == 0).
  constexpr uint32_t kNonPropMask = 2u | 4u | 8u | 16384u | 1048576u;
  constexpr uint32_t kPropFadeMask = 128u | 256u | 512u;
  if ((flags & kNonPropMask) != 0u) return false;
  if (decal != 0u && (flags & kPropFadeMask) == 0u) return false;
  // Compact individual props (signs, bollards, mailboxes, bushes, small fence posts, tall slender poles).
  if (ex <= 12.0f && ez <= 12.0f && ey <= 24.0f) return true;
  // Merged roadside wooden/wire fence & barrier batches: low vertical profile
  // (<= 4.5 m) extending up to 64 m along the roadside (buildings are taller).
  if (ey <= 4.5f && std::max(ex, ez) <= 64.0f) return true;
  return false;
}

size_t Showroom::hide_world_props_near(const std::array<float, 3>& game_pos, float radius_m, bool persistent) {
  if (radius_m <= 0.0f) return 0;
  const float r = std::max(0.5f, radius_m);
  size_t hidden_spans = 0;

  // 1. Exact per-instance GPU index masking in the resident arena (covers 100% of 64m & 256m
  //    batched prop draws across opaque, cutout, decal, blended, Z-prepass, shadows, and
  //    reflections with zero collateral damage to neighboring intact props).
  if (arena_on()) {
    std::vector<uint64_t> resident_keys;
    resident_keys.reserve(world_draws_.size() + (fading_on_ ? fading_.draws.size() : 0));
    for (const auto& d : world_draws_) {
      if (d.resident && d.index_count > 0 && d.cull) resident_keys.push_back(d.key);
    }
    if (fading_on_) {
      for (const auto& d : fading_.draws) {
        if (d.resident && d.index_count > 0 && d.cull) resident_keys.push_back(d.key);
      }
    }
    std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
    auto& state = g_smash_hide[this];
    hidden_spans = state.hide_near(
        game_pos, r, persistent, resident_keys,
        [this](uint64_t key, uint32_t& base) { return arena_lookup_draw(key, base); },
        [this](uint32_t first, uint32_t count) { gpu_world_->queue_index_zero(first, count); });
  }

  if (world_draws_.empty()) return hidden_spans;

  // 2. Fallback draw-level hiding for non-arena / CPU draws (or if no arena span matched).
  size_t hidden_draws = 0;
  if (!arena_on() || hidden_spans == 0) {
    const float sx = float(double(game_pos[0]) - world_origin_[0]);
    const float sy = game_pos[1];
    const float sz = float(double(game_pos[2]) - world_origin_[1]);
    for (auto& d : world_draws_) {
      if (d.index_count == 0 || !d.cull) continue;
      if (arena_on() && d.resident) {
        // Never nuke a multi-prop 256m resident batch via draw-level hiding if it has tracked spans.
        std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
        auto it = g_smash_hide.find(this);
        if (it != g_smash_hide.end() && it->second.by_draw_key.count(d.key)) continue;
      }
      const uint32_t flags = uint32_t(std::lround(d.push.params[1]));
      const float ex = d.box_max[0] - d.box_min[0];
      const float ey = d.box_max[1] - d.box_min[1];
      const float ez = d.box_max[2] - d.box_min[2];
      if (!is_smashable_prop_draw(flags, d.decal, ex, ey, ez)) continue;
      const float cx = std::clamp(sx, d.box_min[0], d.box_max[0]);
      const float cy = std::clamp(sy, d.box_min[1], d.box_max[1]);
      const float cz = std::clamp(sz, d.box_min[2], d.box_max[2]);
      const float dx = sx - cx, dy = sy - cy, dz = sz - cz;
      if (dx * dx + dz * dz <= r * r && std::fabs(dy) <= r + 1.5f) {
        d.hidden_index_count = d.index_count;
        d.index_count = 0;
        ++hidden_draws;
      }
    }
    if (hidden_draws > 0 && arena_on()) {
      std::string table_err;
      (void)build_gpu_table(table_err);
    }
  }
  return hidden_spans + hidden_draws;
}

size_t Showroom::unhide_world_props_near(const std::array<float, 3>& game_pos, float radius_m) {
  if (radius_m <= 0.0f) return 0;
  const float r = std::max(0.5f, radius_m);
  size_t restored_spans = 0;

  if (arena_on()) {
    std::vector<uint64_t> resident_keys;
    resident_keys.reserve(world_draws_.size() + (fading_on_ ? fading_.draws.size() : 0));
    for (const auto& d : world_draws_) {
      if (d.resident && d.index_count > 0 && d.cull) resident_keys.push_back(d.key);
    }
    if (fading_on_) {
      for (const auto& d : fading_.draws) {
        if (d.resident && d.index_count > 0 && d.cull) resident_keys.push_back(d.key);
      }
    }
    std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
    if (auto sit = g_smash_hide.find(this); sit != g_smash_hide.end()) {
      restored_spans = sit->second.unhide_near(
          game_pos, r, resident_keys,
          [this](uint64_t key, uint32_t& base) { return arena_lookup_draw(key, base); },
          [this](uint32_t first, const std::vector<uint32_t>& indices) {
            gpu_world_->queue_index_restore(first, indices);
          });
    }
  }

  if (world_draws_.empty()) return restored_spans;

  size_t restored_draws = 0;
  const float sx = float(double(game_pos[0]) - world_origin_[0]);
  const float sy = game_pos[1];
  const float sz = float(double(game_pos[2]) - world_origin_[1]);
  for (auto& d : world_draws_) {
    if (d.index_count != 0 || d.hidden_index_count == 0 || !d.cull) continue;
    const float cx = std::clamp(sx, d.box_min[0], d.box_max[0]);
    const float cy = std::clamp(sy, d.box_min[1], d.box_max[1]);
    const float cz = std::clamp(sz, d.box_min[2], d.box_max[2]);
    const float dx = sx - cx, dy = sy - cy, dz = sz - cz;
    if (dx * dx + dz * dz <= r * r && std::fabs(dy) <= r + 1.5f) {
      {
        std::lock_guard<std::mutex> lock(g_smash_hide_mutex);
        if (auto sit = g_smash_hide.find(this); sit != g_smash_hide.end()) {
          if (sit->second.box_overlaps_persistent_sphere(d.box_min, d.box_max, world_origin_[0], world_origin_[1]))
            continue;
        }
      }
      d.index_count = d.hidden_index_count;
      d.hidden_index_count = 0;
      ++restored_draws;
    }
  }
  if (restored_draws > 0 && arena_on()) {
    std::string table_err;
    (void)build_gpu_table(table_err);
  }
  return restored_spans + restored_draws;
}

bool Showroom::build_gpu_table(std::string& error) {
  for (auto& d : world_draws_) d.gpu = false;
  gpu_table_white_ = 0;
  gpu_table_layer_fallback_ = 0;
  active_gpu_table_records_.clear();
  gpu_table_missing_textures_.clear();
  if (!arena_on()) return true;
  std::vector<gpuworld::DrawGpu> rows;
  rows.reserve(world_draws_.size());
  uint32_t per_batch[gpuworld::kBatchCount] = {};
  const bool single_sample = r_.samples_ == VK_SAMPLE_COUNT_1_BIT;
  const bool instanced_trees = !world_trees_.empty();
  std::unordered_map<uint32_t, uint32_t> slot_cache;
  slot_cache.reserve(2048);
  active_gpu_table_records_.reserve(1024);
  auto resolve_slot = [&](uint32_t record, bool& present) -> uint32_t {
    if (record == kNoWorldTexture) {
      present = true;
      return WorldGpu::kDefaultWhite;
    }
    if (auto cit = slot_cache.find(record); cit != slot_cache.end()) {
      present = (cit->second != UINT32_MAX);
      return present ? cit->second : WorldGpu::kDefaultWhite;
    }
    uint32_t s = UINT32_MAX;
    if (gpu_world_->has_texture(record)) {
      s = gpu_world_->texture_slot(record);
    } else {
      // Re-sync any texture that is resident in world_textures_ but missed a bindless slot earlier.
      VkImageView view = VK_NULL_HANDLE;
      {
        std::lock_guard<std::mutex> lock(world_textures_mutex_);
        if (auto it = world_textures_.find(record); it != world_textures_.end()) view = it->second.view;
      }
      if (view) {
        const uint32_t added = gpu_world_->add_texture(record, view);
        if (added != WorldGpu::kDefaultWhite) s = added;
      }
    }
    slot_cache.emplace(record, s);
    if (s != UINT32_MAX) {
      active_gpu_table_records_.push_back(record);
      present = true;
      return s;
    }
    gpu_table_missing_textures_.insert(record);
    present = false;
    return WorldGpu::kDefaultWhite;
  };
  for (auto& d : world_draws_) {
    if (!d.resident || !d.cull || d.index_count == 0 || !gpuworld::in_gpu_batches(d.blend)) continue;
    uint32_t flags = uint32_t(std::lround(d.push.params[1]));
    if ((flags & 2u) != 0) continue;  // the sky dome
    if (instanced_trees && (flags & 32u) != 0) {
      d.gpu = true;  // drawn via single hardware-instanced tree pass
      continue;
    }
    // Keep opaque LOD-dithered props in the opaque batch (never coverage_, which enables
    // hardware alpha-to-coverage at MSAA and would discard matte props whose .a carries surf_gloss=0).
    // WorldGpu::record_draw() binds opaque_dither_ (alphaToCoverage=false, full RGBA mask, shader LOD discard).
    const gpuworld::Batch batch = gpuworld::batch_of(d.blend, d.decal != 0);
    if (per_batch[batch] >= WorldGpu::kMaxPerBatch || rows.size() >= WorldGpu::kMaxTableRows) continue;  // left to the CPU loop
    bool has_tex0 = false, has_texb = false, has_splat = false, has_lmap = false;
    bool has_nmap = false, has_ao = false, has_layc = false, has_spec = false;
    const uint32_t s_tex0 = resolve_slot(d.texture, has_tex0);
    const uint32_t s_texb = resolve_slot(d.texture_b, has_texb);
    const uint32_t s_splat = resolve_slot(d.splat, has_splat);
    const uint32_t s_lmap = resolve_slot(d.light_map, has_lmap);
    const uint32_t s_nmap = resolve_slot(d.normal_map, has_nmap);
    const uint32_t s_ao = resolve_slot(d.ao_map, has_ao);
    const uint32_t s_layc = resolve_slot(d.layer_c, has_layc);
    const uint32_t s_spec = resolve_slot(d.spec_map, has_spec);

    gpuworld::DrawGpu row{};
    for (int k = 0; k < 4; ++k) row.tint[k] = d.push.tint[k];

    // A texture that did not upload: clear only the flags whose textures are missing.
    // Never draw a billboard without its cutout texture (would render as a solid opaque card).
    if (!has_tex0) {
      if ((flags & 32u) != 0u) {
        d.gpu = true;  // suppress fallback CPU draw as well
        continue;
      }
      if (d.texture != kNoWorldTexture) ++gpu_table_white_;
      if ((flags & 1u) != 0u) {
        constexpr uint32_t kAnyTexMask = 1u | 4u | 8u | 16384u | 2048u | 4096u | 8192u | 524288u | 2097152u |
                                         4194304u | 8388608u;
        const bool is_road = (flags & 1048576u) != 0u;
        flags &= ~kAnyTexMask;
        // Replace UV tiling in row.tint with a neutral road/terrain linear albedo instead of pure white.
        if (is_road) {
          row.tint[0] = 0.055f;
          row.tint[1] = 0.054f;
          row.tint[2] = 0.052f;
        } else {
          row.tint[0] = 0.13f;
          row.tint[1] = 0.11f;
          row.tint[2] = 0.085f;
        }
        row.tint[3] = 1.0f;
      }
    } else {
      if ((flags & 4u) != 0u && (d.texture_b == kNoWorldTexture || !has_texb)) {
        flags &= ~4u;
        ++gpu_table_layer_fallback_;
      }
      if ((flags & 8u) != 0u &&
          (d.texture_b == kNoWorldTexture || !has_texb || d.splat == kNoWorldTexture || !has_splat)) {
        flags &= ~(8u | 16384u);
        ++gpu_table_layer_fallback_;
      }
      if ((flags & 16384u) != 0u && (d.layer_c == kNoWorldTexture || !has_layc)) {
        flags &= ~16384u;
        ++gpu_table_layer_fallback_;
      }
      const bool dual_em_lm = (flags & 2048u) != 0u && (flags & 8388608u) != 0u;
      if (dual_em_lm) {
        if (d.light_map == kNoWorldTexture || !has_lmap) flags &= ~2048u;
        if (d.layer_c == kNoWorldTexture || !has_layc) flags &= ~8388608u;
      } else if ((flags & (2048u | 8388608u)) != 0u && (d.light_map == kNoWorldTexture || !has_lmap)) {
        flags &= ~(2048u | 8388608u);
      }
      if (d.normal_map == kNoWorldTexture || !has_nmap) flags &= ~4096u;
      if (d.ao_map == kNoWorldTexture || !has_ao) flags &= ~8192u;
      if ((flags & 2097152u) != 0u && (d.splat == kNoWorldTexture || !has_splat)) flags &= ~2097152u;
      if ((flags & 32768u) != 0u && (d.texture_b == kNoWorldTexture || !has_texb)) flags &= ~4194304u;
      if ((flags & 1048576u) != 0u && (d.spec_map == kNoWorldTexture || !has_spec)) flags &= ~4194304u;
      if (d.spec_map == kNoWorldTexture || !has_spec) flags &= ~524288u;
    }
    if (d.blend == 1 && single_sample) flags |= 64u;  // cut-outs without MSAA: world.frag's alpha test
    row.params[0] = d.push.params[0];
    row.params[1] = float(flags);
    row.params[2] = d.push.params[2];
    row.params[3] = d.push.params[3];
    row.tex0[0] = (d.texture != kNoWorldTexture && has_tex0) ? s_tex0 : WorldGpu::kDefaultWhite;
    row.tex0[1] = (d.texture_b != kNoWorldTexture && has_texb) ? s_texb : WorldGpu::kDefaultWhite;
    row.tex0[2] = (d.splat != kNoWorldTexture && has_splat) ? s_splat : WorldGpu::kDefaultWhite;
    // If a dual _EMIS+_LM draw lost its Light_Map (2048u cleared) while keeping EmissiveMap (8388608u in layer_c),
    // route s_layc into tex0[3] so the shader's emissive-only path samples the window mask.
    const bool dual_em_fallback = (d.light_map == kNoWorldTexture || !has_lmap) &&
                                  (flags & 8388608u) != 0u && (flags & 2048u) == 0u &&
                                  d.layer_c != kNoWorldTexture && has_layc;
    row.tex0[3] = dual_em_fallback ? s_layc
                                   : ((d.light_map != kNoWorldTexture && has_lmap) ? s_lmap : WorldGpu::kDefaultBlack);
    row.tex1[0] = (d.normal_map != kNoWorldTexture && has_nmap) ? s_nmap : WorldGpu::kDefaultFlatNormal;
    row.tex1[1] = (d.ao_map != kNoWorldTexture && has_ao) ? s_ao : WorldGpu::kDefaultWhite;
    row.tex1[2] = (d.layer_c != kNoWorldTexture && has_layc) ? s_layc : WorldGpu::kDefaultWhite;
    const uint32_t spec_slot = (d.spec_map != kNoWorldTexture && has_spec) ? s_spec : WorldGpu::kDefaultWhite;
    row.tex1[3] = (spec_slot << 1u) | (d.fade_in ? 1u : 0u);
    for (int k = 0; k < 3; ++k) row.box_min[k] = d.box_min[k], row.box_max[k] = d.box_max[k];
    row.draw[0] = d.index_count;
    row.draw[1] = d.first_index;
    std::memcpy(&row.draw[2], &d.vertex_offset, sizeof(int32_t));
    row.draw[3] = batch;
    row.shift[0] = d.shift[0];
    row.shift[2] = d.shift[1];
    rows.push_back(row);
    ++per_batch[batch];
    d.gpu = true;
  }
  std::vector<gpuworld::TreeInstanceGpu> gpu_trees;
  if (instanced_trees) {
    struct TreeTmpl {
      uint32_t row_idx = 0;
      uint32_t tex_slot = WorldGpu::kDefaultWhite;
      bool valid = false;
    };
    std::unordered_map<uint32_t, TreeTmpl> tmpl_by_tex;
    gpu_trees.reserve(world_trees_.size());
    for (const auto& t : world_trees_) {
      if (t.texture == kNoWorldTexture) continue;
      auto [it, added] = tmpl_by_tex.emplace(t.texture, TreeTmpl{});
      if (added) {
        bool present = false;
        const uint32_t tslot = resolve_slot(t.texture, present);
        if (present && rows.size() < WorldGpu::kMaxTableRows) {
          it->second = {uint32_t(rows.size()), tslot, true};
          gpuworld::DrawGpu row{};
          // Enable kFlagLodFar (256u) so GPU tree billboards smoothly cross-fade via v_fade in world_tree_gpu.vert.
          uint32_t fl = 1u | 32u | 256u;
          if (single_sample) fl |= 64u;
          row.tint[0] = row.tint[1] = row.tint[2] = row.tint[3] = 1.0f;
          row.params[0] = 1.0f;  // coverage
          row.params[1] = float(fl);
          row.params[3] = 1.0f;
          row.tex0[0] = tslot;
          row.tex0[1] = WorldGpu::kDefaultWhite;
          row.tex0[2] = WorldGpu::kDefaultWhite;
          row.tex0[3] = WorldGpu::kDefaultBlack;
          row.tex1[0] = WorldGpu::kDefaultFlatNormal;
          row.tex1[1] = WorldGpu::kDefaultWhite;
          row.tex1[2] = WorldGpu::kDefaultWhite;
          row.tex1[3] = WorldGpu::kDefaultWhite << 1u;
          row.draw[0] = 0u;  // template-only row: skipped by world_cull.comp
          row.draw[3] = gpuworld::kCutout;
          rows.push_back(row);
        }
      }
      if (!it->second.valid) continue;
      gpuworld::TreeInstanceGpu gt{};
      gt.base_hw[0] = t.base[0], gt.base_hw[1] = t.base[1], gt.base_hw[2] = t.base[2], gt.base_hw[3] = t.half_width;
      gt.up_h[0] = t.up[0], gt.up_h[1] = t.up[1], gt.up_h[2] = t.up[2], gt.up_h[3] = t.height;
      for (int k = 0; k < 4; ++k) gt.uv_rect[k] = t.uv[k];
      gt.col_tex[0] = t.rgba8;
      gt.col_tex[1] = it->second.tex_slot;
      gt.col_tex[2] = it->second.row_idx;
      gpu_trees.push_back(gt);
    }
  }
  return gpu_world_->set_table(rows, error, gpu_trees);
}

// Before the scene: the culling pass over the table, with the view's side planes in the scenery's frame.
void Showroom::record_world_cull(VkCommandBuffer cmd) {
  sync_motion_scene();
  if (!arena_on()) return;
  gpuworld::CullView view;
  float clip[16];
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += frame_.view_projection[k * 4 + row] * world_transform_[col * 4 + k];
      clip[col * 4 + row] = s;
    }
  for (int col = 0; col < 4; ++col) {
    const float x = clip[col * 4 + 0], y = clip[col * 4 + 1], w = clip[col * 4 + 3];
    view.planes[0][col] = w + x, view.planes[1][col] = w - x, view.planes[2][col] = w + y, view.planes[3][col] = w - y;
  }
  const auto eye = eye_in_scenery(world_transform_);
  for (int k = 0; k < 3; ++k) view.eye[k] = eye[size_t(k)];
  view.lod_reach = world_fade_[1] > 0 ? world_fade_[0] + world_fade_[1] + effective_near_prop_reach() : 0.0f;
  view.frustum = world_cull_;
  view.hiz = hiz_enabled_ && have_prev_vp_ && gpu_world_->hiz_valid();
  view.reverse_z = r_.reverse_z();
  view.hiz_width = WorldGpu::kHizWidth;
  view.hiz_height = WorldGpu::kHizHeight;
  std::memcpy(view.prev_vp, prev_vp_, sizeof prev_vp_);
  gpu_world_->record_cull(cmd, view);
  r_.mark(cmd, "hiz_cull");
  std::memcpy(prev_vp_, clip, sizeof prev_vp_);
  have_prev_vp_ = true;
}


}  // namespace carview
