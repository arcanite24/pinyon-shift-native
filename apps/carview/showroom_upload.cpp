#include "showroom_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace carview {

bool Showroom::upload(std::string& error) {
  const modern::WorldMemoryBudget mem_budget =
      modern::choose_world_memory_budget(r_.device_local_bytes(), low_memory_mode_, reflection_rate_);
  dyn_env_size_ = mem_budget.dyn_env_size;
  dyn_env_mips_ = std::min(mem_budget.dyn_env_mips, kDynEnvMips);
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    if (!world_budget_explicit_) {
      uint64_t max_bytes = mem_budget.texture_lru_bytes;
      size_t max_count = mem_budget.texture_lru_count;
      if (uint64_t mb = 0; parse_env_u64("SHIFT_WORLD_TEX_BUDGET_MB", mb) &&
                            mb <= (std::numeric_limits<uint64_t>::max() >> 20))
        max_bytes = mb << 20;
      if (uint64_t cnt = 0; parse_env_u64("SHIFT_WORLD_TEX_MAX_COUNT", cnt) &&
                            cnt <= std::numeric_limits<size_t>::max())
        max_count = size_t(cnt);
      if (max_count > 0 && r_.gpu_driven_supported()) {
        max_count = std::min<size_t>(max_count, WorldGpu::kMaxTextures - 128u);
      }
      world_lru_.set_budget(max_bytes, max_count);
    }
  }
  const auto pipe_t0 = std::chrono::steady_clock::now();
  if (!r_.ensure_shadow_map(error) || !create_pipelines(error)) return false;
  pipeline_compile_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipe_t0).count();
  const bool shadows = r_.options_.shadow.size > 0;
  if (shadows && (!r_.create_buffer(sizeof(ShowroomFrame),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shadow_frame_buffer_) ||
                  !r_.create_buffer(sizeof(ShowroomFrame),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shadow_frame_mid_buffer_) ||
                  !r_.create_buffer(sizeof(ShowroomFrame),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shadow_frame_far_buffer_))) {
    error = "shadow frame buffer";
    return false;
  }
  const VkBufferUsageFlags rt_geom_flags =
      r_.rt_supported()
          ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
          : 0u;
  // Batch vertex + index upload into one staging buffer and a single submit_once
  // to halve synchronous GPU fence waits when spawning cars / traffic / AI guests.
  bool geom_uploaded = false;
  {
    const VkDeviceSize vb_bytes = vertices_.size() * sizeof(bake::Vertex);
    const VkDeviceSize ib_bytes = indices_.size() * sizeof(uint32_t);
    const VkDeviceSize ib_off = (vb_bytes + 15ull) & ~15ull;
    const VkDeviceSize total_bytes = ib_off + ib_bytes;
    Renderer::Buffer staging;
    if (total_bytes > 0 &&
        r_.create_buffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
      void* mapped = nullptr;
      if (vmaMapMemory(r_.allocator_, staging.allocation, &mapped) == VK_SUCCESS) {
        auto* dst = static_cast<uint8_t*>(mapped);
        if (vb_bytes > 0) std::memcpy(dst, vertices_.data(), size_t(vb_bytes));
        if (ib_bytes > 0) std::memcpy(dst + ib_off, indices_.data(), size_t(ib_bytes));
        vmaUnmapMemory(r_.allocator_, staging.allocation);
        if (r_.create_buffer(vb_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_geom_flags,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertex_buffer_) &&
            r_.create_buffer(ib_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_geom_flags,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, index_buffer_)) {
          geom_uploaded = r_.submit_once([&](VkCommandBuffer cmd) {
            if (vb_bytes > 0) {
              VkBufferCopy vc{0, 0, vb_bytes};
              vkCmdCopyBuffer(cmd, staging.buffer, vertex_buffer_.buffer, 1, &vc);
            }
            if (ib_bytes > 0) {
              VkBufferCopy ic{ib_off, 0, ib_bytes};
              vkCmdCopyBuffer(cmd, staging.buffer, index_buffer_.buffer, 1, &ic);
            }
          });
        }
      }
      r_.destroy_buffer(staging);
    }
  }
  if (!geom_uploaded ||
      !r_.create_buffer(sizeof(ShowroomFrame),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame_buffer_) ||
      !r_.create_buffer(sizeof(HeadlightUbo),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, headlights_ubo_) ||
      (!guest_ && !r_.create_buffer(kMaxOverlayVertices * sizeof(WorldVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, overlay_vertices_)) ||
      (!guest_ && !create_dyn_buffer(car_inst_buf_, kMaxCarInstances * sizeof(CarInstanceGpu), error,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) ||
      (guest_ && !r_.create_buffer(sizeof(CarInstanceGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, empty_inst_ssbo_))) {
    error = "showroom buffers";
    return false;
  }
  {
    bake::Texture flat_n;
    flat_n.format = bake::TextureFormat::kRgba8;
    flat_n.levels.push_back({1, 1, {128, 128, 255, 255}});
    bake::Texture def_cookie;
    def_cookie.format = bake::TextureFormat::kRgba8;
    def_cookie.levels.push_back({1, 1, {255, 255, 255, 255}});
    bake::Texture black;
    black.name = "@black";
    black.levels.push_back({1, 1, {0, 0, 0, 255}});
    bake::Texture fb_dirt_p, fb_dirt_m, fb_glass;
    fb_dirt_p.format = fb_dirt_m.format = fb_glass.format = bake::TextureFormat::kRgba8;
    fb_dirt_p.levels.push_back({1, 1, {96, 74, 52, 255}});
    fb_dirt_m.levels.push_back({1, 1, {180, 180, 180, 255}});
    fb_glass.levels.push_back({1, 1, {0, 0, 0, 0}});

    auto find_shared = [&](const char* name) -> const bake::Texture* {
      for (const auto& st : shared_.textures)
        if (st.name == name) return &st;
      return nullptr;
    };

    gpu_textures_.resize(textures_.size());
    std::vector<TextureUploadItem> batch;
    batch.reserve(textures_.size() + 5);
    batch.push_back({&flat_n, false, &flat_normal_tex_, false});
    batch.push_back({&def_cookie, false, &headlight_cookie_, false});
    for (size_t i = 0; i < textures_.size(); ++i) {
      if (i == 1) {
        batch.push_back({&black, false, &gpu_textures_[i], false});
        continue;
      }
      const auto* t = textures_[i].texture;
      bool srgb = true;
      if (t) {
        if (t->format == bake::TextureFormat::kBc4 || t->format == bake::TextureFormat::kBc5 ||
            t->format == bake::TextureFormat::kR8) {
          srgb = false;
        } else {
          const std::string tn = lower(t->name);
          if (tn.find("_nrm") != std::string::npos || tn.find("normal") != std::string::npos ||
              tn.find("_bump") != std::string::npos || tn.find("dirt_mask") != std::string::npos ||
              tn.find("sparkle") != std::string::npos || tn.find("_occ") != std::string::npos ||
              tn == "livery_mask") {
            srgb = false;
          }
        }
      }
      batch.push_back({t, srgb, &gpu_textures_[i], true});
    }
    const bake::Texture* sd_p = find_shared("dirt_pattern.xds");
    const bake::Texture* sd_m = find_shared("dirt_mask.xds");
    const bake::Texture* sd_g = find_shared("glass_texture.xds");
    batch.push_back({sd_p ? sd_p : &fb_dirt_p, sd_p != nullptr, &shared_dirt_pattern_, true});
    batch.push_back({sd_m ? sd_m : &fb_dirt_m, false, &shared_dirt_mask_, true});
    batch.push_back({sd_g ? sd_g : &fb_glass, false, &shared_glass_crack_, true});
    if (!upload_textures_batch(batch, error)) return false;
  }
  if (!upload_environment(error)) return false;
  if (!guest_ && !init_dynamic_reflections(error)) return false;

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler.maxLod = VK_LOD_CLAMP_NONE;
  if (r_.sampler_anisotropy() > 1.0f) {
    sampler.anisotropyEnable = VK_TRUE;
    sampler.maxAnisotropy = r_.sampler_anisotropy();
  }
  VK_TRY(vkCreateSampler(r_.device_, &sampler, nullptr, &sampler_), "vkCreateSampler");

  // One material set per distinct (slot 0, slot 1, livery, normal_map, damage_map).
  for (auto& d : draws_) {
    auto [it, inserted] = descriptor_by_textures_.emplace(d.textures, uint32_t(descriptor_by_textures_.size()));
    d.descriptor = it->second;
  }
  const uint32_t sets = uint32_t(descriptor_by_textures_.size());
  // Frame/offscreen pool (kept alive across car hot-swaps, with spare sets for zero-stall rebinding).
  const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 36},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 208},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 16;
  pool.poolSizeCount = 3;
  pool.pPoolSizes = sizes;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool");
  const VkDescriptorPoolSize mat_sizes[1] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(1u, sets) * 5}};
  VkDescriptorPoolCreateInfo mat_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  mat_pool.maxSets = std::max(1u, sets);
  mat_pool.poolSizeCount = 1;
  mat_pool.pPoolSizes = mat_sizes;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &mat_pool, nullptr, &material_pool_), "material descriptor pool");
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &frame_layout_;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &frame_set_), "frame set");
  if (shadows) {
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &shadow_set_), "shadow frame set");
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &shadow_set_mid_), "shadow mid frame set");
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &shadow_set_far_), "shadow far frame set");
  }
  if (!guest_ && dyn_env_ready_) {
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &env_frame_set_), "env frame set");
    if (env_prefilter_set_layout_ && dyn_env_mip0_cube_) {
      VkDescriptorSetAllocateInfo palloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      palloc.descriptorPool = pool_;
      palloc.descriptorSetCount = 1;
      palloc.pSetLayouts = &env_prefilter_set_layout_;
      VK_TRY(vkAllocateDescriptorSets(r_.device_, &palloc, &env_prefilter_set_), "env prefilter set");
      VkDescriptorImageInfo pinfo{sampler_, dyn_env_mip0_cube_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet pw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      pw.dstSet = env_prefilter_set_;
      pw.dstBinding = 0;
      pw.descriptorCount = 1;
      pw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      pw.pImageInfo = &pinfo;
      vkUpdateDescriptorSets(r_.device_, 1, &pw, 0, nullptr);
    }
  }
  // Frame sets: main, shadow near, shadow mid, shadow far, and offscreen env/mirror.
  for (VkDescriptorSet target : {frame_set_, shadow_set_, shadow_set_mid_, shadow_set_far_, env_frame_set_}) {
    if (!target) continue;
    VkBuffer ubo = frame_buffer_.buffer;
    if (target == shadow_set_) ubo = shadow_frame_buffer_.buffer;
    else if (target == shadow_set_mid_) ubo = shadow_frame_mid_buffer_.buffer;
    else if (target == shadow_set_far_) ubo = shadow_frame_far_buffer_.buffer;
    else if (target == env_frame_set_) ubo = env_frame_buffer_.buffer;
    VkDescriptorBufferInfo buffer{ubo, 0, sizeof(ShowroomFrame)};
    // Main frame binds live dyn_env_ & mirror_tex_ once active; guests inherit host's dyn_env_ at upload time
    // to avoid any post-upload vkDeviceWaitIdle stall.
    VkImageView cube_view = environment_.view;
    VkSampler cube_sampler = sampler_;
    if (target == frame_set_) {
      if (guest_ && shared_dyn_env_view_ && shared_dyn_env_sampler_) {
        cube_view = shared_dyn_env_view_;
        cube_sampler = shared_dyn_env_sampler_;
        bound_dyn_env_view_ = shared_dyn_env_view_;
      } else if (dyn_env_ready_ && r_.post()) {
        cube_view = dyn_env_.view;
        dyn_env_bound_ = true;
      }
    }
    const VkImageView mirror_view =
        (target == frame_set_ && dyn_env_ready_ && mirror_tex_.view) ? mirror_tex_.view : gpu_textures_[1].view;
    VkDescriptorImageInfo cube{cube_sampler, cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo static_cube{sampler_, environment_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map{r_.shadow_sampler_, r_.shadow_map_.view,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map_raw{r_.shadow_raw_sampler_, r_.shadow_map_.view,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo hl_buf{headlights_ubo_.buffer, 0, sizeof(HeadlightUbo)};
    VkDescriptorImageInfo mir{sampler_, mirror_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo inst_buf{guest_ ? empty_inst_ssbo_.buffer : car_inst_buf_.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo dirt_p{sampler_, shared_dirt_pattern_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dirt_m{sampler_, shared_dirt_mask_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo gcrack{sampler_, shared_glass_crack_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[12]{};
    for (int wi = 0; wi < 12; ++wi) {
      writes[wi] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[wi].dstSet = target;
      writes[wi].dstBinding = uint32_t(wi);
      writes[wi].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &buffer;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &cube;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &map;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &cookie;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &hl_buf;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &mir;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].pBufferInfo = &inst_buf;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].pImageInfo = &static_cube;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[8].pImageInfo = &dirt_p;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].pImageInfo = &dirt_m;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[10].pImageInfo = &gcrack;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[11].pImageInfo = &map_raw;
    vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
  }
  material_sets_.resize(sets);
  std::vector<VkDescriptorSetLayout> layouts(sets, material_layout_);
  alloc.descriptorPool = material_pool_;
  alloc.descriptorSetCount = sets;
  alloc.pSetLayouts = layouts.data();
  if (sets > 0) {
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, material_sets_.data()), "material sets");
  }
  for (const auto& [textures, index] : descriptor_by_textures_) {
    VkDescriptorImageInfo images[5];
    VkWriteDescriptorSet writes[5];
    for (uint32_t b = 0; b < 5; ++b) {
      VkImageView view = VK_NULL_HANDLE;
      if (b < 3) {
        view = gpu_textures_[textures[b]].view;
      } else if (b == 3) {
        view = (textures[3] != UINT32_MAX && textures[3] < gpu_textures_.size())
                   ? gpu_textures_[textures[3]].view
                   : flat_normal_tex_.view;
      } else {
        view = (textures[4] != UINT32_MAX && textures[4] < gpu_textures_.size())
                   ? gpu_textures_[textures[4]].view
                   : gpu_textures_[textures[0]].view;
      }
      images[b] = {sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[b].dstSet = material_sets_[index];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(r_.device_, 5, writes, 0, nullptr);
  }

  if (!guest_ && r_.gpu_driven_supported() && !std::getenv("SHIFT_NO_ARENA")) {
    // The GPU-driven world: arenas for the resident draws, bindless textures, compute culling.
    WorldGpu::Setup gs;
    gs.frame_layout = frame_layout_;
    gs.sampler = sampler_;
    gs.white = gpu_textures_[0].view;
    gs.black = gpu_textures_[1].view;
    gs.flat_normal = flat_normal_tex_.view;
    uint64_t vb_bytes = mem_budget.vertex_arena_bytes;
    if (uint64_t mb = 0; parse_env_u64("SHIFT_WORLD_ARENA_MB", mb) && mb >= 16 && mb <= 2048) vb_bytes = mb << 20;
    gs.vertex_capacity = uint32_t(vb_bytes / sizeof(WorldVertex));
    gs.index_capacity = gs.vertex_capacity * 4;
    gs.retire = [this](std::function<void()> f) { retire(std::move(f)); };
    auto gw = std::make_unique<WorldGpu>(r_);
    std::string gerr;
    const auto wg_t0 = std::chrono::steady_clock::now();
    if (gw->init(gs, gerr)) {
      gpu_world_ = std::move(gw);
      if (auto hv = gpu_world_->hiz_raw_view()) r_.set_hiz_preview_view(hv);
    } else {
      notes_.push_back("GPU-driven world off: " + gerr);
    }
    pipeline_compile_ms_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wg_t0).count();
  }
  if (guest_) return true;
  upload_ring_.r = &r_;
  upload_budget_ms_ = 1.8;  // <= 2.0 ms/frame streaming upload budget (override via SHIFT_UPLOAD_BUDGET_MS)
  if (const char* ub = std::getenv("SHIFT_UPLOAD_BUDGET_MS"); ub && *ub) {
    const double v = std::atof(ub);
    if (v >= 0.0 && v <= 50.0) upload_budget_ms_ = v;
  }
  // Eagerly pre-compile ALL RT/PT compute pipelines & RT scene during startup when RT is enabled
  // (eliminates 100+ ms Frame 0 lazy shader compilation & seq_ib submit_once stall).
  if (r_.rt_supported() && r_.active_rt_mode() != rt::RtMode::kOff) {
    std::string rt_err;
    const auto rt_t0 = std::chrono::steady_clock::now();
    if (!precompile_rt_pipelines(rt_err)) {
      notes_.push_back("RT startup pre-compile note: " + rt_err);
    }
    pipeline_compile_ms_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rt_t0).count();
  }
  SceneHooks hooks;
  hooks.before = [this](VkCommandBuffer cmd) {
    ++world_frame_;
    release_retired();
    pump_deferred_uploads(cmd);
    if (world_lit_ && draw_car_) {
      // The drive: the car takes the world's sun and sky (showroom.frag).
      for (int k = 0; k < 3; ++k) frame_.key_light[k] = world_sun_[k], frame_.drive[k] = world_up_[k];
      frame_.key_light[3] = 1.0f;
      frame_.fill_light[3] = 0.0f;
      frame_.options[0] = 1.0f;
      frame_.drive[3] = 1.0f;
      for (int k = 0; k < 3; ++k) frame_.tint[k] = frame_.reflect_tint[k] = 1.0f;
    } else {
      frame_.drive[3] = 0.0f;
    }
    apply_tod_to_frame();
    // Game-space basis in render space (columns 0..2 of world_transform_) + real time for water ripples.
    for (int k = 0; k < 3; ++k) {
      frame_.env_basis_x[k] = world_transform_[0 + k];
      frame_.env_basis_y[k] = world_transform_[4 + k];
      frame_.env_basis_z[k] = world_transform_[8 + k];
    }
    frame_.env_basis_x[3] = tod_real_time_;
    frame_.env_basis_y[3] = wind_speed_;
    frame_.env_basis_z[3] = wind_amplitude_;
    if (sky_ready_) record_sky_before(cmd);
    record_lights_before(cmd);
    const bool shadows = shadows_on();
    if (shadows) {
      place_shadow();
    } else {
      std::memset(frame_.shadow_matrix, 0, sizeof frame_.shadow_matrix);
      std::memset(frame_.shadow_matrix_mid, 0, sizeof frame_.shadow_matrix_mid);
      std::memset(frame_.shadow_matrix_far, 0, sizeof frame_.shadow_matrix_far);
      std::memset(frame_.shadow, 0, sizeof frame_.shadow);
      std::memset(frame_.shadow_bias, 0, sizeof frame_.shadow_bias);
      frame_.shadow_far_bias[0] = frame_.shadow_far_bias[1] = 0.0f;
      frame_.shadow_mid_bias[0] = frame_.shadow_mid_bias[1] = frame_.shadow_mid_bias[2] = 0.0f;
      frame_.shadow_mid_bias[3] = 2.0f;
      shadow_drawn_ = shadow_cars_ = 0;
    }
    // Cockpit near-clip distance in shadow_bias.y prevents exterior lamp/streetlamp glows bleeding into cockpit.
    frame_.shadow_bias[1] = cockpit_camera_active_ ? 2.1f : 0.0f;
    // Mikkelsen Hex-Tiling toggle in shadow_bias.z (organic Colorado terrain breakup on high/ultra).
    frame_.shadow_bias[2] = r_.graphics_settings().hex_tiling ? 1.0f : 0.0f;
    // Phase B SOTA frame params: car detail, brake rotor thermal glow, temporal jitter, SSR active.
    frame_.phase_b_params[0] = r_.graphics_settings().car_detail ? 1.0f : 0.0f;
    frame_.phase_b_params[1] = rotor_glow_;
    frame_.phase_b_params[2] = float(r_.frames_submitted() & 63u) / 64.0f;
    frame_.phase_b_params[3] = (r_.graphics_settings().ssr != modern::SsrQuality::kOff) ? 1.0f : 0.0f;
    // Frame-exact sun screen-space projection for optical god rays & lens flare in post.frag
    // plus view-space sun direction (valid at all camera angles) for Screen-Space Contact Shadows (SSCS).
    if (world_lit_ && r_.options_.post.enabled) {
      const float sx = world_sun_[0], sy = world_sun_[1], sz = world_sun_[2];
      const float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
      if (slen > 1e-4f) {
        const float dx = sx / slen, dy = sy / slen, dz = sz / slen;
        const float px = frame_.eye[0] + dx * 1000.0f;
        const float py = frame_.eye[1] + dy * 1000.0f;
        const float pz = frame_.eye[2] + dz * 1000.0f;
        const float* vp = frame_.view_projection;
        const float cx = vp[0] * px + vp[4] * py + vp[8] * pz + vp[12];
        const float cy = vp[1] * px + vp[5] * py + vp[9] * pz + vp[13];
        const float cw = vp[3] * px + vp[7] * py + vp[11] * pz + vp[15];
        const float sun_up = dx * world_up_[0] + dy * world_up_[1] + dz * world_up_[2];
        const float elev_fade = std::clamp((sun_up + 0.02f) / 0.10f, 0.0f, 1.0f);
        // Reconstruct unit view-space sun direction from P * (V * dir).
        const float cdx = vp[0] * dx + vp[4] * dy + vp[8] * dz;
        const float cdy = vp[1] * dx + vp[5] * dy + vp[9] * dz;
        const float cdw = vp[3] * dx + vp[7] * dy + vp[11] * dz;
        const float p00 = std::fabs(r_.cam_proj00_) > 1e-4f ? r_.cam_proj00_ : 1.0f;
        const float p11 = std::fabs(r_.cam_proj11_) > 1e-4f ? r_.cam_proj11_ : -1.0f;
        const float svx = cdx / p00, svy = cdy / p11, svz = -cdw;
        const float svlen = std::sqrt(svx * svx + svy * svy + svz * svz);
        if (svlen > 1e-5f) {
          r_.options_.post.sun_view_dir[0] = svx / svlen;
          r_.options_.post.sun_view_dir[1] = svy / svlen;
          r_.options_.post.sun_view_dir[2] = svz / svlen;
        }
        r_.options_.post.sun_elev_vis = elev_fade;
        if (cw > 1e-4f && elev_fade > 0.001f) {
          const float nx = cx / cw, ny = cy / cw;
          r_.options_.post.sun_screen_uv[0] = nx * 0.5f + 0.5f;
          r_.options_.post.sun_screen_uv[1] = ny * 0.5f + 0.5f;
          const float off = std::max(std::fabs(nx) - 0.85f, std::fabs(ny) - 0.85f);
          const float edge_fade = std::clamp(1.0f - std::max(off, 0.0f) / 0.70f, 0.0f, 1.0f);
          r_.options_.post.sun_visibility = elev_fade * edge_fade;
        } else {
          r_.options_.post.sun_visibility = 0.0f;
        }
        // Populate Phase B Volumetric Sun Shafts / Aerial Perspective / Night Headlight Cones state.
        if (r_.graphics_settings().volumetric_fog != modern::VolumetricFog::kOff) {
          Renderer::VolumetricState vs{};
          // General 4x4 matrix inverse for column-major frame_.view_projection.
          const float* m = frame_.view_projection;
          float inv[16];
          inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
          inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
          inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
          inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
          inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
          inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
          inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
          inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
          inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
          inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
          inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
          inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
          inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
          inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
          inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
          inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
          const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
          if (std::fabs(det) > 1e-12f) {
            const float inv_det = 1.0f / det;
            for (int k = 0; k < 16; ++k) vs.inv_view_proj[k] = inv[k] * inv_det;
          }
          std::memcpy(vs.shadow_matrix, frame_.shadow_matrix, sizeof vs.shadow_matrix);
          std::memcpy(vs.shadow_matrix_mid, frame_.shadow_matrix_mid, sizeof vs.shadow_matrix_mid);
          std::memcpy(vs.shadow_matrix_far, frame_.shadow_matrix_far, sizeof vs.shadow_matrix_far);
          vs.eye_time[0] = frame_.eye[0];
          vs.eye_time[1] = frame_.eye[1];
          vs.eye_time[2] = frame_.eye[2];
          vs.eye_time[3] = tod_real_time_;
          vs.sun_dir_vis[0] = dx;
          vs.sun_dir_vis[1] = dy;
          vs.sun_dir_vis[2] = dz;
          vs.sun_dir_vis[3] = elev_fade;
          const bool has_tod = frame_.sun_color[3] >= 0.0f;
          vs.sun_col_str[0] = has_tod ? std::max(frame_.fog_scatter[0], frame_.sun_color[0] * 0.75f) : 1.0f;
          vs.sun_col_str[1] = has_tod ? std::max(frame_.fog_scatter[1], frame_.sun_color[1] * 0.65f) : 0.80f;
          vs.sun_col_str[2] = has_tod ? std::max(frame_.fog_scatter[2], frame_.sun_color[2] * 0.50f) : 0.50f;
          vs.sun_col_str[3] = std::max(r_.options_.post.god_ray_strength, 0.42f);
          vs.fog_col_dens[0] = has_tod ? frame_.fog_color[0] : 0.09f;
          vs.fog_col_dens[1] = has_tod ? frame_.fog_color[1] : 0.14f;
          vs.fog_col_dens[2] = has_tod ? frame_.fog_color[2] : 0.15f;
          vs.fog_col_dens[3] = has_tod ? std::clamp(frame_.fog_params[0] * 2.2f, 0.0005f, 0.006f) : 0.0012f;
          vs.shadow_p[0] = frame_.shadow[0];
          vs.shadow_p[1] = frame_.shadow_mid_bias[1];
          vs.shadow_p[2] = vol_night_factor_;
          vs.shadow_p[3] = float(vol_beam_count_);
          vs.up_eyey[0] = world_up_[0];
          vs.up_eyey[1] = world_up_[1];
          vs.up_eyey[2] = world_up_[2];
          vs.up_eyey[3] = tod_eye_game_y_;
          vs.height_p[0] = frame_.fog_params[1];
          vs.height_p[1] = frame_.fog_params[2];
          vs.height_p[2] = frame_.fog_params[3];
          vs.height_p[3] = 0.0f;
          for (uint32_t bi = 0; bi < vol_beam_count_ && bi < 8u; ++bi) vs.beams[bi] = vol_beams_[bi];
          r_.set_volumetric_state(vs);
        }
      }
    }
    if (world_lit_ && dyn_env_ready_) {
      record_env_and_mirror(cmd);
    }
    frame_.shadow_far_bias[2] = (dyn_env_bound_ && dyn_env_initialized_) ? 1.0f : 0.0f;
    frame_.shadow_far_bias[3] = mirror_valid_ ? 1.0f : 0.0f;
    update_frame(cmd);
    record_world_cull(cmd);
    // Pack traffic / AI car instances for hardware instancing (main pass + shadow pass).
    if (!instances_.empty() && car_inst_buf_.ready()) {
      auto* dst = static_cast<CarInstanceGpu*>(car_inst_buf_.data());
      std::vector<Showroom*> groups;
      for (const auto& in : instances_) {
        if (in.car && std::find(groups.begin(), groups.end(), in.car) == groups.end()) groups.push_back(in.car);
      }
      uint32_t written = 0;
      for (Showroom* g : groups) {
        auto write_one = [&](const Instance& in) {
          if (written >= kMaxCarInstances) return;
          CarInstanceGpu& ci = dst[written++];
          std::memcpy(ci.model, in.model, sizeof ci.model);
          for (int k = 0; k < 4; ++k) ci.stripe[k] = g->stripe_[k];
          const float fa = std::clamp(in.fade_alpha, 0.0f, 1.0f);
          ci.pad[0] = fa;
          ci.pad[1] = ci.pad[2] = ci.pad[3] = 0.0f;
          const drive::LampShaderVectors& active_lamps = in.has_lamps ? in.lamps : g->lamp_vectors_;
          for (uint32_t ch = 0; ch < 30; ++ch) {
            if (ch == 0) {
              if (in.has_paint) {
                ci.lamp_rgb[0][0] = in.paint_rgb[0];
                ci.lamp_rgb[0][1] = in.paint_rgb[1];
                ci.lamp_rgb[0][2] = in.paint_rgb[2];
                ci.lamp_rgb[0][3] = 1.0f;
              } else {
                ci.lamp_rgb[0][0] = ci.lamp_rgb[0][1] = ci.lamp_rgb[0][2] = ci.lamp_rgb[0][3] = 0.0f;
              }
            } else {
              const auto rgb = drive::evaluate_lamp_emission(static_cast<drive::LampChannel>(ch), active_lamps,
                                                             g->lamp_colors_);
              ci.lamp_rgb[ch][0] = rgb[0] * fa, ci.lamp_rgb[ch][1] = rgb[1] * fa, ci.lamp_rgb[ch][2] = rgb[2] * fa,
              ci.lamp_rgb[ch][3] = 1.0f;
            }
          }
        };
        // Pack fully opaque instances first (fast batched draw), then fading instances.
        for (const auto& in : instances_)
          if (in.car == g && in.fade_alpha >= 0.99f) write_one(in);
        for (const auto& in : instances_)
          if (in.car == g && in.fade_alpha < 0.99f) write_one(in);
      }
      ++car_inst_buf_.ver;
      dyn_sync(car_inst_buf_, VkDeviceSize(written) * sizeof(CarInstanceGpu));
    } else if (!instances_.empty()) {
      // Fallback per-guest frame upload when instanced SSBO is unavailable.
      std::vector<Showroom*> seen;
      for (const auto& in : instances_) {
        if (!in.car || std::find(seen.begin(), seen.end(), in.car) != seen.end()) continue;
        seen.push_back(in.car);
        in.car->frame_ = frame_;
        for (int i = 0; i < 4; ++i) in.car->frame_.stripe[i] = in.car->stripe_[i];
        const bool guest_has_dyn =
            dyn_env_bound_ && dyn_env_initialized_ && dyn_env_.view && in.car->bound_dyn_env_view_ == dyn_env_.view;
        in.car->frame_.shadow_far_bias[2] = guest_has_dyn ? 1.0f : 0.0f;
        in.car->frame_.shadow_far_bias[3] = 0.0f;
        in.car->update_frame(cmd);
        if (shadows && in.car->shadow_set_) {
          in.car->shadow_frame_ = shadow_frame_;
          in.car->update_buffer(cmd, in.car->shadow_frame_buffer_.buffer, in.car->shadow_frame_);
          if (in.car->shadow_set_mid_) {
            in.car->shadow_frame_mid_ = shadow_frame_mid_;
            in.car->update_buffer(cmd, in.car->shadow_frame_mid_buffer_.buffer, in.car->shadow_frame_mid_);
          }
          if (in.car->shadow_set_far_) {
            in.car->shadow_frame_far_ = shadow_frame_far_;
            in.car->update_buffer(cmd, in.car->shadow_frame_far_buffer_.buffer, in.car->shadow_frame_far_);
          }
        }
      }
    }
    if (shadows) {
      update_buffer(cmd, shadow_frame_buffer_.buffer, shadow_frame_);
      if (shadow_frame_mid_buffer_.buffer)
        update_buffer(cmd, shadow_frame_mid_buffer_.buffer, shadow_frame_mid_);
      if (shadow_frame_far_buffer_.buffer)
        update_buffer(cmd, shadow_frame_far_buffer_.buffer, shadow_frame_far_);
      record_shadow(cmd);
      r_.mark(cmd, "shadows");
    }
    if (r_.active_rt_mode() != rt::RtMode::kOff) {
      if (!rt_scene_ && !rt_init_failed_) {
        auto sc = std::make_unique<RtScene>(r_);
        std::string rt_err;
        if (sc->init([this](std::function<void()> fn) { retire(std::move(fn)); }, rt_err)) {
          rt_scene_ = std::move(sc);
        } else {
          rt_init_failed_ = true;
          notes_.push_back("RT scene init failed: " + rt_err);
        }
      }
      if (rt_scene_ && rt_scene_->ready()) {
        rt_scene_->record_update(cmd, *this);
        r_.mark(cmd, "rt_bvh");
      }
    }
    if (vfx_ready_ && particle_set_ && r_.depth_resolved_view() && r_.depth_resolved_view() != particle_bound_depth_) {
      particle_bound_depth_ = r_.depth_resolved_view();
      VkDescriptorImageInfo depth_info{particle_sampler_, particle_bound_depth_,
                                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wd.dstSet = particle_set_;
      wd.dstBinding = 1;
      wd.descriptorCount = 1;
      wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wd.pImageInfo = &depth_info;
      vkUpdateDescriptorSets(r_.device_, 1, &wd, 0, nullptr);
    }
  };
  hooks.draw_z_prepass = [this](VkCommandBuffer cmd) {
    record_z_prepass(cmd);
  };
  hooks.draw = [this](VkCommandBuffer cmd) {
    car_draws_ = 0;
    r_.mark(cmd, "setup");
    record_world_opaque(cmd);
    record_skidmarks_draw(cmd);
    record_contact_shadows(cmd);
    r_.mark(cmd, "contact_shadow");
    record_debris_draw(cmd);
    record_showcase_draw(cmd);
    record_driver_draw(cmd);
    const VkPipeline car_op = (r_.z_prepass() && opaque_after_z_) ? opaque_after_z_ : opaque_;
    if (draw_car_) record_draws(cmd, car_op, glass_, 1);
    record_all_instances(cmd, r_.z_prepass(), 1);
    if (world_textured_ && sky_ready_) {
      record_sky_draw(cmd);
      r_.mark(cmd, "sky");
    }
    record_world_blended(cmd);
    if (draw_car_) record_draws(cmd, car_op, glass_, 2);
    record_all_instances(cmd, r_.z_prepass(), 2);
    record_glows_draw(cmd);
    r_.mark(cmd, "car_glass");
  };
  hooks.has_particles = [this]() -> bool {
    return vfx_ready_ && (particle_alpha_count_ > 0 || particle_add_count_ > 0);
  };
  hooks.draw_particles = [this](VkCommandBuffer cmd) {
    record_particles_draw(cmd);
  };
  hooks.build_hiz = [this](VkCommandBuffer cmd, VkImageView dv, uint32_t w, uint32_t h, bool rev) {
    if (arena_on() && gpu_world_) gpu_world_->record_hiz(cmd, dv, w, h, rev);
  };
  hooks.record_rt = [this](VkCommandBuffer cmd, VkImage hdr_img, VkImageView hdr_view, VkImageView depth_view,
                           VkImageView motion_view, uint32_t w, uint32_t h) {
    if (r_.active_rt_mode() == rt::RtMode::kOff || !rt_scene_ || !rt_scene_->ready()) return;
    if (!rt_pipeline_ && !rt_init_failed_) {
      auto pipe = std::make_unique<RtPipeline>(r_);
      VkDescriptorSetLayout bl_layout = gpu_world_ ? gpu_world_->bindless_layout() : VK_NULL_HANDLE;
      VkImageView fb_white = !gpu_textures_.empty() ? gpu_textures_[0].view : VK_NULL_HANDLE;
      std::string rt_err;
      if (pipe->init(bl_layout, sampler_, fb_white, rt_err,
                     [this](std::function<void()> fn) { retire(std::move(fn)); })) {
        rt_pipeline_ = std::move(pipe);
      } else {
        rt_init_failed_ = true;
        notes_.push_back("RT pipeline init failed: " + rt_err);
      }
    }
    if (rt_pipeline_ && rt_pipeline_->ready()) {
      rt_pipeline_->record(cmd, *this, *rt_scene_, hdr_img, hdr_view, depth_view, motion_view, w, h);
    }
  };
  hooks.clear[0] = 0.09f;
  hooks.clear[1] = 0.095f;
  hooks.clear[2] = 0.105f;
  r_.set_scene(hooks);
  return true;
}


bool Showroom::create_dyn_buffer(DynBuffer& d, VkDeviceSize cap, std::string& error, VkBufferUsageFlags usage) {
  const VkMemoryPropertyFlags host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  d.cap = cap;
  if (!r_.create_buffer(cap * Renderer::kFramesInFlight, usage, host_flags, d.buf) ||
      vmaMapMemory(r_.allocator_, d.buf.allocation, &d.gpu) != VK_SUCCESS) {
    error = "dynamic vertex buffer";
    d.gpu = nullptr;
    return false;
  }
  d.cpu.assign(size_t(cap), 0);
  d.ver = 1;
  for (auto& v : d.slot_ver) v = 0;
  return true;
}

void Showroom::destroy_dyn_buffer(DynBuffer& d) {
  if (d.gpu) vmaUnmapMemory(r_.allocator_, d.buf.allocation);
  d.gpu = nullptr;
  r_.destroy_buffer(d.buf);
  d.cpu.clear();
}

VkDeviceSize Showroom::dyn_sync(DynBuffer& d, VkDeviceSize used_bytes) {
  const uint32_t slot = r_.frame_slot();
  const VkDeviceSize at = d.cap * slot;
  if (d.slot_ver[slot] != d.ver && d.gpu) {
    // This slot's previous frame is done (draw_frame waited its fence).
    std::memcpy(static_cast<uint8_t*>(d.gpu) + at, d.cpu.data(), size_t(std::min(used_bytes, d.cap)));
    d.slot_ver[slot] = d.ver;
  }
  return at;
}

Showroom::UploadRingGuard::~UploadRingGuard() {
  if (!r || !r->allocator_) return;
  for (auto& s : slots) {
    if (s.mapped) {
      vmaUnmapMemory(r->allocator_, s.staging.allocation);
      s.mapped = nullptr;
    }
    r->destroy_buffer(s.staging);
    s.capacity = 0;
  }
}

bool Showroom::init_minimal_for_precompile() {
  vertices_.assign(3, bake::Vertex{});
  indices_ = {0, 1, 2};
  textures_.clear();
  textures_.push_back({nullptr});  // 0: built-in white
  textures_.push_back({nullptr});  // 1: built-in black
  draws_.clear();
  Draw d{};
  d.first_index = 0;
  d.index_count = 3;
  d.textures = {0, 0, 1, UINT32_MAX, UINT32_MAX};
  draws_.push_back(d);
  return true;
}

bool Showroom::precompile_rt_pipelines(std::string& error) {
  if (!r_.rt_supported()) return true;
  if (!rt_scene_ && !rt_init_failed_) {
    auto sc = std::make_unique<RtScene>(r_);
    if (sc->init([this](std::function<void()> fn) { retire(std::move(fn)); }, error)) {
      rt_scene_ = std::move(sc);
    } else {
      rt_init_failed_ = true;
      return false;
    }
  }
  if (!rt_pipeline_ && !rt_init_failed_) {
    auto pipe = std::make_unique<RtPipeline>(r_);
    VkDescriptorSetLayout bl_layout = gpu_world_ ? gpu_world_->bindless_layout() : VK_NULL_HANDLE;
    VkImageView fb_white = !gpu_textures_.empty() ? gpu_textures_[0].view : VK_NULL_HANDLE;
    if (pipe->init(bl_layout, sampler_, fb_white, error,
                   [this](std::function<void()> fn) { retire(std::move(fn)); })) {
      rt_pipeline_ = std::move(pipe);
    } else {
      rt_init_failed_ = true;
      return false;
    }
  }
  return true;
}

bool Showroom::prewarm_rt_targets(uint32_t width, uint32_t height) {
  if (!rt_pipeline_ || !rt_pipeline_->ready()) return true;
  return rt_pipeline_->prewarm_targets(width, height);
}

void Showroom::queue_deferred_world_texture(std::shared_ptr<const WorldTextureData> tex) {
  if (!tex) return;
  std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
  deferred_world_textures_.push_back(std::move(tex));
}

size_t Showroom::deferred_upload_queue_size() const {
  std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
  return deferred_world_textures_.size();
}

void Showroom::pump_deferred_uploads(VkCommandBuffer cmd) {
  {
    std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
    if (deferred_world_textures_.empty()) return;
  }
  constexpr VkDeviceSize kDefaultRingBytes = 4ull << 20;  // 4 MiB per frame-in-flight ring slot
  const uint32_t slot_idx = r_.frame_slot() % Renderer::kFramesInFlight;
  UploadRingSlot& ring = upload_ring_.slots[slot_idx];
  if (!ring.mapped) {
    if (!r_.create_buffer(kDefaultRingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ring.staging) ||
        vmaMapMemory(r_.allocator_, ring.staging.allocation, &ring.mapped) != VK_SUCCESS) {
      ring.mapped = nullptr;
      r_.destroy_buffer(ring.staging);
      return;
    }
    ring.capacity = kDefaultRingBytes;
  }
  constexpr VkDeviceSize kMaxFrameBytes = 2ull << 20;  // 2 MiB/frame GPU transfer cap to prevent queue spikes
  constexpr uint32_t kMaxTexturesPerFrame = 12;
  const auto budget_t0 = std::chrono::steady_clock::now();
  VkDeviceSize cursor = 0;
  uint32_t uploaded_count = 0;
  std::vector<uint32_t> uploaded_records;
  uploaded_records.reserve(kMaxTexturesPerFrame);
  for (;;) {
    if (uploaded_count >= kMaxTexturesPerFrame) break;
    if (upload_budget_ms_ > 0.0 && uploaded_count > 0) {
      const double elapsed_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - budget_t0).count();
      if (elapsed_ms >= upload_budget_ms_) break;
    }
    std::shared_ptr<const WorldTextureData> item;
    {
      std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
      if (deferred_world_textures_.empty()) break;
      item = deferred_world_textures_.front();
    }
    if (!item || item->texture.levels.empty()) {
      std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
      if (!deferred_world_textures_.empty()) deferred_world_textures_.pop_front();
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      if (world_textures_.count(item->record)) {
        std::lock_guard<std::mutex> qlock(deferred_uploads_mutex_);
        if (!deferred_world_textures_.empty()) deferred_world_textures_.pop_front();
        continue;
      }
    }
    VkDeviceSize total = 0;
    const uint32_t levels = texture_chain(item->texture, total);
    if (!levels) {
      std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
      if (!deferred_world_textures_.empty()) deferred_world_textures_.pop_front();
      continue;
    }
    const uint32_t skip = modern::texture_levels_to_skip(item->texture.levels[0].width, item->texture.levels[0].height,
                                                         levels, texture_cap_);
    VkDeviceSize kept = 0;
    for (uint32_t l = skip; l < levels; ++l) kept += item->texture.levels[l].data.size();
    const VkDeviceSize aligned_cursor = (cursor + 15ull) & ~15ull;
    if (cursor > 0 && aligned_cursor + kept > kMaxFrameBytes) break;
    if (aligned_cursor + kept > ring.capacity) {
      if (cursor > 0) break;  // Defer to next frame so we never stall or exceed ring capacity.
      // Single texture larger than 4 MiB: grow this slot's ring buffer cleanly (slot fence already signaled).
      vmaUnmapMemory(r_.allocator_, ring.staging.allocation);
      ring.mapped = nullptr;
      r_.destroy_buffer(ring.staging);
      ring.capacity = 0;
      const VkDeviceSize new_cap = std::max(kDefaultRingBytes, (kept + 65535ull) & ~65535ull);
      if (!r_.create_buffer(new_cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ring.staging) ||
          vmaMapMemory(r_.allocator_, ring.staging.allocation, &ring.mapped) != VK_SUCCESS) {
        ring.mapped = nullptr;
        r_.destroy_buffer(ring.staging);
        break;
      }
      ring.capacity = new_cap;
    }
    {
      std::lock_guard<std::mutex> lock(deferred_uploads_mutex_);
      if (!deferred_world_textures_.empty()) deferred_world_textures_.pop_front();
    }
    VkDeviceSize write_at = aligned_cursor;
    auto* dst = static_cast<uint8_t*>(ring.mapped);
    for (uint32_t l = skip; l < levels; ++l) {
      const auto& d = item->texture.levels[l].data;
      std::memcpy(dst + write_at, d.data(), d.size());
      write_at += d.size();
    }
    std::string why;
    if (!begin_world_texture(*item, ring.staging.buffer, aligned_cursor, levels, skip, cmd, nullptr, why)) {
      notes_.push_back("deferred world texture " + item->texture.name + ": " + why);
    } else {
      world_staging_bytes_.fetch_add(uint64_t(kept), std::memory_order_relaxed);
      uploaded_records.push_back(item->record);
      ++uploaded_count;
    }
    cursor = write_at;
  }
  if (uploaded_count > 0 && arena_on()) {
    bool needs_table_refresh = (gpu_table_white_ > 0 || gpu_table_layer_fallback_ > 0);
    if (!needs_table_refresh && !gpu_table_missing_textures_.empty()) {
      for (uint32_t rec : uploaded_records) {
        if (gpu_table_missing_textures_.count(rec)) {
          needs_table_refresh = true;
          break;
        }
      }
    }
    if (needs_table_refresh) {
      std::string table_err;
      (void)build_gpu_table(table_err);
    }
  }
}


}  // namespace carview
