#include "showroom_internal.h"

#include "world_gpu_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace carview {

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
    if (fading_on_)
      for (const auto& d : fading_.draws) pin_draw(d);
    if (pending_world_) {
      for (const auto& d : pending_world_->staged.draws) pin_draw(d);
      for (const auto& t : pending_world_->staged.textures)
        if (t) world_lru_.pin(t->record, epoch);
    }
    for (const auto& [record, count] : prepare_pins_)
      if (count > 0) world_lru_.pin(record, epoch);
    std::vector<uint32_t> victims;
    world_lru_.evict_unpinned(epoch, victims);
    if (victims.empty()) return;
    evicted_images.reserve(victims.size());
    for (uint32_t record : victims) {
      if (gpu_world_) gpu_world_->remove_texture(record);
      if (auto it = world_textures_.find(record); it != world_textures_.end()) {
        evicted_images.push_back(it->second);
        world_textures_.erase(it);
      }
      if (auto it = world_sets_.find(record); it != world_sets_.end()) {
        evicted_sets.push_back(it->second);
        world_sets_.erase(it);
      }
      for (auto it = world_layer_sets_.begin(); it != world_layer_sets_.end();) {
        const auto& k = it->first;
        bool match = false;
        for (uint32_t r_id : k)
          if (r_id == record) match = true;
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

void Showroom::world_release_draws(std::vector<WorldGeometry::Draw>& draws) {
  if (!arena_on()) return;
  const uint64_t tag = r_.frames_submitted();
  for (auto& d : draws) {
    if (!d.resident) continue;
    gpu_world_->resident().release(d.key, tag);
    d.resident = false;
  }
}

// Gives each draw of a pending world its place in the arenas: found by key (a regather that left it as it was
// uploads nothing) or placed, its vertices and indices copied from the staging buffers. When the arenas are full
// of scenery in use, the GPU is waited for, the old scenery given up and this one placed afresh.
bool Showroom::place_world_arena(StagedWorld& st, std::vector<WorldGpu::Copy>& vertex_copies,
                                 std::vector<WorldGpu::Copy>& index_copies, std::string& error) {
  auto& res = gpu_world_->resident();
  std::vector<uint32_t> staged_first(st.draws.size());
  for (size_t i = 0; i < st.draws.size(); ++i) staged_first[i] = st.draws[i].first_index;
  std::vector<const gpuworld::ResidentDraw*> placed(st.draws.size(), nullptr);
  const uint64_t generation = ++world_generation_;
  auto attempt = [&]() {
    vertex_copies.clear();
    index_copies.clear();
    for (size_t i = 0; i < st.draws.size(); ++i) {
      auto& d = st.draws[i];
      placed[i] = nullptr;
      if (d.vertex_count == 0 || d.index_count == 0) continue;
      const gpuworld::ResidentDraw* rd = res.acquire(d.key, generation);
      const bool known = rd != nullptr;
      if (!rd)
        rd = res.place(d.key, d.vertex_count, d.index_count, st.origin_x, st.origin_z, generation, r_.frames_done());
      if (!rd) {
        for (size_t k = 0; k < i; ++k)
          if (placed[k]) res.release(st.draws[k].key, 0);
        return false;
      }
      placed[i] = rd;
      if (!known) {
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
  if (!attempt()) {
    r_.wait_idle();
    drop_fading_world(true);
    world_release_draws(world_draws_);
    world_draws_.clear();
    release_retired();
    res.evict_unreferenced(r_.frames_done());
    if (!attempt()) {
      error = "world arena full";
      return false;
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

// The table the culling pass reads: one row per resident opaque or cut-out draw,
// plus template rows for hardware-instanced tree sprites.
bool Showroom::is_smashable_prop_draw(uint32_t flags, uint8_t decal, float ex, float ey, float ez) {
  // Never hide sky dome (2), terrain layers (4 vertex-blend, 8 splat, 16384 layer-C),
  // roads (1048576), or projected road decals.
  constexpr uint32_t kNonPropMask = 2u | 4u | 8u | 16384u | 1048576u;
  if ((flags & kNonPropMask) != 0u || decal != 0u) return false;
  // Compact individual props (signs, bollards, mailboxes, bushes, small fence posts).
  if (ex <= 12.0f && ey <= 12.0f && ez <= 12.0f) return true;
  // Merged roadside wooden/wire fence & barrier batches: low vertical profile
  // (<= 4.5 m) extending up to 64 m along the roadside (buildings are taller).
  if (ey <= 4.5f && std::max(ex, ez) <= 64.0f) return true;
  return false;
}

size_t Showroom::hide_world_props_near(const std::array<float, 3>& game_pos, float radius_m) {
  if (world_draws_.empty() || radius_m <= 0.0f) return 0;
  // World draw AABBs live in the scenery-relative frame (shifted by world_origin_).
  const float sx = float(double(game_pos[0]) - world_origin_[0]);
  const float sy = game_pos[1];
  const float sz = float(double(game_pos[2]) - world_origin_[1]);
  const float r = std::max(0.5f, radius_m);
  size_t hidden = 0;
  for (auto& d : world_draws_) {
    if (d.index_count == 0 || !d.cull) continue;
    const uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
    const float ex = d.box_max[0] - d.box_min[0];
    const float ey = d.box_max[1] - d.box_min[1];
    const float ez = d.box_max[2] - d.box_min[2];
    if (!is_smashable_prop_draw(flags, d.decal, ex, ey, ez)) continue;
    // Closest point on draw AABB to smashed prop sphere.
    const float cx = std::clamp(sx, d.box_min[0], d.box_max[0]);
    const float cy = std::clamp(sy, d.box_min[1], d.box_max[1]);
    const float cz = std::clamp(sz, d.box_min[2], d.box_max[2]);
    const float dx = sx - cx, dy = sy - cy, dz = sz - cz;
    if (dx * dx + dz * dz <= r * r && std::fabs(dy) <= r + 1.5f) {
      d.hidden_index_count = d.index_count;
      d.index_count = 0;
      ++hidden;
    }
  }
  if (hidden > 0 && arena_on()) {
    std::string table_err;
    (void)build_gpu_table(table_err);
  }
  return hidden;
}

size_t Showroom::unhide_world_props_near(const std::array<float, 3>& game_pos, float radius_m) {
  if (world_draws_.empty() || radius_m <= 0.0f) return 0;
  const float sx = float(double(game_pos[0]) - world_origin_[0]);
  const float sy = game_pos[1];
  const float sz = float(double(game_pos[2]) - world_origin_[1]);
  const float r = std::max(0.5f, radius_m);
  size_t restored = 0;
  for (auto& d : world_draws_) {
    if (d.index_count != 0 || d.hidden_index_count == 0 || !d.cull) continue;
    const float cx = std::clamp(sx, d.box_min[0], d.box_max[0]);
    const float cy = std::clamp(sy, d.box_min[1], d.box_max[1]);
    const float cz = std::clamp(sz, d.box_min[2], d.box_max[2]);
    const float dx = sx - cx, dy = sy - cy, dz = sz - cz;
    if (dx * dx + dz * dz <= r * r && std::fabs(dy) <= r + 1.5f) {
      d.index_count = d.hidden_index_count;
      d.hidden_index_count = 0;
      ++restored;
    }
  }
  if (restored > 0 && arena_on()) {
    std::string table_err;
    (void)build_gpu_table(table_err);
  }
  return restored;
}

bool Showroom::build_gpu_table(std::string& error) {
  for (auto& d : world_draws_) d.gpu = false;
  gpu_table_white_ = 0;
  gpu_table_layer_fallback_ = 0;
  if (!arena_on()) return true;
  std::vector<gpuworld::DrawGpu> rows;
  rows.reserve(world_draws_.size());
  uint32_t per_batch[gpuworld::kBatchCount] = {};
  const bool single_sample = r_.samples_ == VK_SAMPLE_COUNT_1_BIT;
  const bool instanced_trees = !world_trees_.empty();
  auto have = [&](uint32_t record) { return record == kNoWorldTexture || gpu_world_->has_texture(record); };
  auto slot = [&](uint32_t record, uint32_t fallback) {
    return record != kNoWorldTexture && gpu_world_->has_texture(record) ? gpu_world_->texture_slot(record) : fallback;
  };
  for (auto& d : world_draws_) {
    if (!d.resident || !d.cull || d.index_count == 0 || !gpuworld::in_gpu_batches(d.blend)) continue;
    uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
    if ((flags & 2u) != 0) continue;  // the sky dome
    if (instanced_trees && (flags & 32u) != 0) {
      d.gpu = true;  // drawn via single hardware-instanced tree pass
      continue;
    }
    // Route LOD-dithered opaque props to the coverage batch (keeping params[0]=0) so
    // kOpaque/kOpaqueDecal run 100% discard-free with Hardware Early-Z enabled.
    const bool lod_dither = (flags & (128u | 256u | 512u)) != 0u;
    const gpuworld::Batch batch = gpuworld::batch_of((d.blend == 0 && lod_dither) ? 1 : d.blend, d.decal != 0);
    if (per_batch[batch] >= WorldGpu::kMaxPerBatch) continue;  // left to the CPU loop
    // A texture that did not upload: the draw falls back as the per-draw sets did.
    // Never draw a billboard without its cutout texture (would render as a solid opaque card).
    if (!have(d.texture)) {
      if ((flags & 32u) != 0u) {
        d.gpu = true;  // suppress fallback CPU draw as well
        continue;
      }
      if (d.texture != kNoWorldTexture) ++gpu_table_white_;
      if (flags == 1) flags = 0;
    } else if ((flags & 32768u) == 0u && d.texture_b != kNoWorldTexture && !(have(d.texture_b) && have(d.splat))) {
      ++gpu_table_layer_fallback_;
      flags = 1;
    }
    if (d.normal_map == kNoWorldTexture || !have(d.normal_map)) flags &= ~4096u;
    if ((flags & 32768u) != 0u && (d.texture_b == kNoWorldTexture || !have(d.texture_b))) flags &= ~4194304u;
    if (d.spec_map == kNoWorldTexture || !have(d.spec_map)) flags &= ~524288u;
    if (d.blend == 1 && single_sample) flags |= 64u;  // cut-outs without MSAA: world.frag's alpha test
    gpuworld::DrawGpu row{};
    for (int k = 0; k < 4; ++k) row.tint[k] = d.push.tint[k];
    row.params[0] = d.push.params[0];
    row.params[1] = float(flags);
    row.params[2] = d.push.params[2];
    row.params[3] = d.push.params[3];
    row.tex0[0] = slot(d.texture, WorldGpu::kDefaultWhite);
    row.tex0[1] = slot(d.texture_b, WorldGpu::kDefaultWhite);
    row.tex0[2] = slot(d.splat, WorldGpu::kDefaultWhite);
    row.tex0[3] = slot(d.light_map, WorldGpu::kDefaultBlack);
    row.tex1[0] = slot(d.normal_map, WorldGpu::kDefaultFlatNormal);
    row.tex1[1] = slot(d.ao_map, WorldGpu::kDefaultWhite);
    row.tex1[2] = slot(d.layer_c, WorldGpu::kDefaultWhite);
    row.tex1[3] = (slot(d.spec_map, WorldGpu::kDefaultWhite) << 1u) | (d.fade_in ? 1u : 0u);
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
    std::unordered_map<uint32_t, uint32_t> tmpl_by_tex;
    gpu_trees.reserve(world_trees_.size());
    for (const auto& t : world_trees_) {
      if (t.texture == kNoWorldTexture || !have(t.texture)) continue;
      auto [it, added] = tmpl_by_tex.emplace(t.texture, uint32_t(rows.size()));
      if (added) {
        gpuworld::DrawGpu row{};
        uint32_t fl = 1u | 32u;
        if (single_sample) fl |= 64u;
        row.tint[0] = row.tint[1] = row.tint[2] = row.tint[3] = 1.0f;
        row.params[0] = 1.0f;  // coverage
        row.params[1] = float(fl);
        row.params[3] = 1.0f;
        row.tex0[0] = slot(t.texture, WorldGpu::kDefaultWhite);
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
      gpuworld::TreeInstanceGpu gt{};
      gt.base_hw[0] = t.base[0], gt.base_hw[1] = t.base[1], gt.base_hw[2] = t.base[2], gt.base_hw[3] = t.half_width;
      gt.up_h[0] = t.up[0], gt.up_h[1] = t.up[1], gt.up_h[2] = t.up[2], gt.up_h[3] = t.height;
      for (int k = 0; k < 4; ++k) gt.uv_rect[k] = t.uv[k];
      gt.col_tex[0] = t.rgba8;
      gt.col_tex[1] = slot(t.texture, WorldGpu::kDefaultWhite);
      gt.col_tex[2] = it->second;
      gpu_trees.push_back(gt);
    }
  }
  return gpu_world_->set_table(rows, error, gpu_trees);
}

// Before the scene: the culling pass over the table, with the view's side planes in the scenery's frame.
void Showroom::record_world_cull(VkCommandBuffer cmd) {
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
  view.lod_reach = world_fade_[1] > 0 ? world_fade_[0] + world_fade_[1] + kPropReach : 0.0f;
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
