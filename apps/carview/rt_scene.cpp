#include "rt_scene.h"

#ifdef SHIFT_ENABLE_RAY_TRACING

#include <algorithm>
#include <cmath>
#include <cstring>

#include <vk_mem_alloc.h>

#include "showroom.h"
#include "world_gpu.h"

namespace carview {

namespace {

void mat4_mul(const float a[16], const float b[16], float out[16]) {
  float r[16];
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      r[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                         a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
    }
  }
  std::memcpy(out, r, sizeof(r));
}

// Converts column-major 4x4 matrix to Vulkan row-major 3x4 VkTransformMatrixKHR.
VkTransformMatrixKHR to_vk_transform(const float m[16]) {
  VkTransformMatrixKHR t{};
  t.matrix[0][0] = m[0];  t.matrix[0][1] = m[4];  t.matrix[0][2] = m[8];   t.matrix[0][3] = m[12];
  t.matrix[1][0] = m[1];  t.matrix[1][1] = m[5];  t.matrix[1][2] = m[9];   t.matrix[1][3] = m[13];
  t.matrix[2][0] = m[2];  t.matrix[2][1] = m[6];  t.matrix[2][2] = m[10];  t.matrix[2][3] = m[14];
  return t;
}

void fill_inst_rows(const float m[16], RtInstanceInfoGpu& info) {
  info.model_row0[0] = m[0];  info.model_row0[1] = m[4];  info.model_row0[2] = m[8];   info.model_row0[3] = m[12];
  info.model_row1[0] = m[1];  info.model_row1[1] = m[5];  info.model_row1[2] = m[9];   info.model_row1[3] = m[13];
  info.model_row2[0] = m[2];  info.model_row2[1] = m[6];  info.model_row2[2] = m[10];  info.model_row2[3] = m[14];
}

void as_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = src_stage;
  mb.srcAccessMask = src_access;
  mb.dstStageMask = dst_stage;
  mb.dstAccessMask = dst_access;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cmd, &dep);
}

// Classifies car material class + flags into RT shader material properties.
void fill_car_geom_material_raw(const ShowroomPush& push, drive::LampChannel lamp_channel, bool brake_lamp,
                                float paint_finish_x, RtGeomInfoGpu& g) {
  const uint32_t klass = uint32_t(std::lround(push.params[0]));
  const uint32_t dflags = uint32_t(std::lround(push.params[1]));
  g.material_class = klass;
  g.flags = kRtGeomFlagCarVertex;
  g.tint[0] = push.tint[0];
  g.tint[1] = push.tint[1];
  g.tint[2] = push.tint[2];
  g.tint[3] = 0.45f;  // default roughness
  for (int k = 0; k < 4; ++k) g.uv0_xform[k] = push.uv0[k];

  if (klass == 1u) {  // kPaint
    g.flags |= kRtGeomFlagClearcoat;
    if ((dflags & 2u) != 0u) g.flags |= kRtGeomFlagMetallic;
    const int finish = int(std::lround(paint_finish_x));
    if (finish == 2) {  // Matte
      g.flags &= ~kRtGeomFlagClearcoat;
      g.tint[3] = 0.58f;
    } else if (finish == 4) {  // Chrome
      g.flags |= kRtGeomFlagMetallic | kRtGeomFlagChrome;
      g.tint[3] = 0.04f;
    } else {
      g.tint[3] = ((dflags & 2u) != 0u) ? 0.24f : 0.08f;
    }
    g.tex_aux = uint32_t(std::clamp(finish, 0, 7));
  } else if (klass == 2u) {  // kGlass
    g.flags |= kRtGeomFlagGlass;
    g.tint[3] = 0.03f;
    // Tag detail_glass_red/amber/clear lamp covers (kFlagCover = 1024u) in tex_aux
    // so PT applies translucent ruby/amber lens tint instead of window glass tint.
    if ((dflags & 1024u) != 0u) {
      const bool is_red = push.tint[0] > 0.15f && push.tint[1] < 0.05f;
      const bool is_amber = push.tint[0] > 0.35f && push.tint[1] > 0.10f && push.tint[2] < 0.05f;
      g.tex_aux = is_red ? 1u : (is_amber ? 2u : 3u);
    } else {
      g.tex_aux = 0u;
    }
  } else if (klass == 3u) {  // kLamp
    g.flags |= kRtGeomFlagChrome;
    // Live emission is evaluated per-frame in record_update() from LampShaderVectors
    // (never baked permanently emissive at BLAS creation time!).
    const bool is_tail_brake = (lamp_channel == drive::LampChannel::kBrake ||
                                lamp_channel == drive::LampChannel::kTail ||
                                lamp_channel == drive::LampChannel::kTailBrake2S ||
                                lamp_channel == drive::LampChannel::kFogRed ||
                                lamp_channel == drive::LampChannel::kDrlRed ||
                                lamp_channel == drive::LampChannel::kDrlBrakeRed || brake_lamp);
    const bool is_amber = (lamp_channel == drive::LampChannel::kIndicatorLeft ||
                           lamp_channel == drive::LampChannel::kIndicatorRight ||
                           lamp_channel == drive::LampChannel::kSideLightOrange ||
                           lamp_channel == drive::LampChannel::kSideIndLeft ||
                           lamp_channel == drive::LampChannel::kSideIndRight ||
                           lamp_channel == drive::LampChannel::kDrlOrange);
    const bool is_head_drl = (lamp_channel == drive::LampChannel::kHeadOld ||
                              lamp_channel == drive::LampChannel::kHeadXenon ||
                              lamp_channel == drive::LampChannel::kHeadHid ||
                              lamp_channel == drive::LampChannel::kFullBeam ||
                              lamp_channel == drive::LampChannel::kFogWhite ||
                              lamp_channel == drive::LampChannel::kDrlWhite ||
                              lamp_channel == drive::LampChannel::kDrlWhiteSwitch);
    if (is_tail_brake) {
      // Unlit red taillight reflector: dark ruby housing (never glowing orange in daylight).
      g.tint[0] = 0.24f;
      g.tint[1] = 0.025f;
      g.tint[2] = 0.020f;
    } else if (is_amber) {
      g.tint[0] = 0.38f;
      g.tint[1] = 0.18f;
      g.tint[2] = 0.025f;
    } else {
      g.tint[0] = 0.62f;
      g.tint[1] = 0.64f;
      g.tint[2] = 0.68f;
    }
    g.tint[3] = 0.08f;
    g.tex_aux = is_tail_brake ? 1u : (is_head_drl ? 2u : 0u);
  } else if (klass == 6u) {  // kTyre
    g.tint[0] = 0.032f;
    g.tint[1] = 0.032f;
    g.tint[2] = 0.035f;
    g.tint[3] = 0.76f;
  } else if (klass == 7u) {  // kRim
    g.flags |= kRtGeomFlagMetallic;
    g.tint[0] = 0.68f;
    g.tint[1] = 0.70f;
    g.tint[2] = 0.74f;
    g.tint[3] = 0.14f;
  } else if (klass == 8u) {  // kCaliper
    g.flags |= kRtGeomFlagMetallic | kRtGeomFlagClearcoat;
    g.tint[0] = 0.68f;
    g.tint[1] = 0.44f;
    g.tint[2] = 0.12f;
    g.tint[3] = 0.22f;
  } else if (klass == 9u) {  // kChrome
    g.flags |= kRtGeomFlagMetallic | kRtGeomFlagChrome;
    g.tint[0] = 0.80f;
    g.tint[1] = 0.82f;
    g.tint[2] = 0.85f;
    g.tint[3] = 0.05f;
  } else if (klass == 10u) {  // kCarbon
    g.flags |= kRtGeomFlagClearcoat;
    g.tint[0] = 0.07f;
    g.tint[1] = 0.07f;
    g.tint[2] = 0.08f;
    g.tint[3] = 0.18f;
  } else if (klass == 4u || klass == 5u) {  // kInterior / kGauge
    // Cool charcoal automotive cabin reflectance so seats, dash, and driver read crisply through rear glass.
    g.tint[0] = 0.095f;
    g.tint[1] = 0.098f;
    g.tint[2] = 0.105f;
    g.tint[3] = 0.72f;
  } else {  // kOpaque (grilles, wheel arches, underbody, rubber trim, diffusers)
    const bool is_grille = (dflags & 256u) != 0u;
    g.tint[0] = is_grille ? 0.035f : 0.055f;
    g.tint[1] = is_grille ? 0.035f : 0.055f;
    g.tint[2] = is_grille ? 0.040f : 0.060f;
    g.tint[3] = is_grille ? 0.36f : 0.58f;
  }
}

struct WorldGeomTexRecordsLocal {
  uint32_t albedo = kNoWorldTexture;
  uint32_t aux = kNoWorldTexture;
  uint32_t splat = kNoWorldTexture;
  uint32_t layer_c = kNoWorldTexture;
  uint32_t ao = kNoWorldTexture;
  bool pending = false;
};

uint32_t resolve_world_tex_slot(const WorldGpu* gpu_world, uint32_t rec, bool& pending) {
  if (!gpu_world || rec == kNoWorldTexture) return 0u;
  if (!gpu_world->has_texture(rec)) {
    pending = true;
    return 0u;
  }
  const uint32_t slot = gpu_world->texture_slot(rec);
  return (slot >= 3u) ? slot : 0u;
}

void fill_world_geom_material(const WorldGeometry::Draw& d, const WorldGpu* gpu_world, RtGeomInfoGpu& g,
                              WorldGeomTexRecordsLocal& refs) {
  const uint32_t wflags = uint32_t(std::lround(d.push.params[1]));
  const bool is_textured = (wflags & 1u) != 0u;
  const bool is_vblend = (wflags & 4u) != 0u;
  const bool is_splat = (wflags & 8u) != 0u;
  const bool is_vfade = (wflags & 16u) != 0u;
  const bool is_ao = (wflags & 8192u) != 0u;
  const bool is_layer_c = (wflags & 16384u) != 0u;
  const bool is_water = (wflags & 32768u) != 0u;
  const bool is_road = (wflags & 1048576u) != 0u;  // kFlagRoad (world_frag.glsl:147)
  const bool is_decal_noise = (wflags & 2097152u) != 0u;
  const bool is_decal = (d.decal != 0u && d.blend >= 2u);
  const bool is_cutout = (d.blend == 1u);
  // Only actual EmissiveMap props (neon signs, lit windows, festival screens) are emissive in PT;
  // never flag roads, splat terrain, or foliage/cutout trees as emissive (breaks SVGF demodulation
  // and turns daytime/sunset foliage lightmaps into nuclear-white crowns!).
  const bool is_emissive_prop =
      (d.light_map != kNoWorldTexture) && ((wflags & 8388608u) != 0u) && !is_cutout && !is_road && !is_splat &&
      !is_vblend;

  g.material_class =
      is_water ? 102u : ((is_road || is_decal) ? 101u : (is_cutout ? 103u : (is_emissive_prop ? 104u : 100u)));
  g.flags = 0u;
  if (is_water) g.flags |= kRtGeomFlagWater;
  if (is_road) g.flags |= kRtGeomFlagRoad;
  if (is_cutout) g.flags |= kRtGeomFlagCutout;
  if (is_emissive_prop) g.flags |= kRtGeomFlagEmissive;
  if (is_vblend) g.flags |= kRtGeomFlagVertexBlend;
  if (is_splat) g.flags |= kRtGeomFlagSplat;
  if (is_layer_c) g.flags |= kRtGeomFlagLayerC;
  if (is_decal) g.flags |= kRtGeomFlagDecal;
  if (is_vfade) g.flags |= kRtGeomFlagVertexFade;
  if (is_decal_noise) g.flags |= kRtGeomFlagDecalNoise;
  if (is_ao && d.ao_map != kNoWorldTexture) g.flags |= kRtGeomFlagAoMap;

  if (is_textured) {
    // For textured world draws, push.tint holds UV tiling (Blend_A xy, Blend_B/decal_noise zw), not RGB tint!
    g.tint[0] = 1.0f;
    g.tint[1] = 1.0f;
    g.tint[2] = 1.0f;
    const float sx = std::fabs(d.push.tint[0]) > 1e-4f ? d.push.tint[0] : 1.0f;
    const float sy = std::fabs(d.push.tint[1]) > 1e-4f ? d.push.tint[1] : 1.0f;
    const float bx = std::fabs(d.push.tint[2]) > 1e-4f ? d.push.tint[2] : sx;
    const float by = std::fabs(d.push.tint[3]) > 1e-4f ? d.push.tint[3] : sy;
    g.uv0_xform[0] = sx;
    g.uv0_xform[1] = sy;
    g.uv0_xform[2] = bx;
    g.uv0_xform[3] = by;
  } else {
    g.tint[0] = d.push.tint[0];
    g.tint[1] = d.push.tint[1];
    g.tint[2] = d.push.tint[2];
    g.uv0_xform[0] = 1.0f;
    g.uv0_xform[1] = 1.0f;
    g.uv0_xform[2] = 1.0f;
    g.uv0_xform[3] = 1.0f;
  }
  g.tint[3] = is_water ? 0.04f : ((is_road || is_decal) ? 0.62f : 0.86f);

  uint32_t packed_p3 = 0u;
  std::memcpy(&packed_p3, &d.push.params[3], sizeof(packed_p3));
  g.extra_tex[2] = packed_p3;

  refs = {};
  refs.albedo = d.texture;
  if (is_vblend || is_splat) refs.aux = d.texture_b;
  else if (is_emissive_prop) refs.aux = d.light_map;
  if (is_splat || is_decal_noise) refs.splat = d.splat;
  if (is_layer_c) refs.layer_c = d.layer_c;
  if (is_ao) refs.ao = d.ao_map;

  g.tex_albedo = resolve_world_tex_slot(gpu_world, refs.albedo, refs.pending);
  if (g.tex_albedo != 0u) g.flags |= kRtGeomFlagBindlessTex;
  g.tex_aux = resolve_world_tex_slot(gpu_world, refs.aux, refs.pending);
  g.extra_tex[0] = resolve_world_tex_slot(gpu_world, refs.splat, refs.pending);
  g.extra_tex[1] = resolve_world_tex_slot(gpu_world, refs.layer_c, refs.pending);
  g.extra_tex[3] = resolve_world_tex_slot(gpu_world, refs.ao, refs.pending);
}

}  // namespace

RtScene::RtScene(Renderer& r) : r_(r) {}

RtScene::~RtScene() { shutdown(); }

bool RtScene::init(RetireFn retire_fn, std::string& error) {
  if (!r_.rt_supported()) {
    error = "ray tracing not supported on this device";
    return false;
  }
  retire_fn_ = std::move(retire_fn);
  VkDevice d = r_.device_;

  const uint32_t align = r_.rt_caps().min_as_scratch_alignment;
  scratch_alloc_.reset_capacity(kBlasScratchBytes, align);
  for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) {
    if (!r_.create_buffer(kBlasScratchBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, blas_scratch_[i])) {
      error = "RT BLAS scratch buffer";
      shutdown();
      return false;
    }
    blas_scratch_bda_[i] = r_.buffer_device_address(blas_scratch_[i].buffer);
  }

  VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  qpi.queryCount = kMaxCompactionQueries;
  if (vkCreateQueryPool(d, &qpi, nullptr, &compact_pool_) != VK_SUCCESS) {
    error = "RT compaction query pool";
    shutdown();
    return false;
  }

  // Upload shared sequential index buffer (0..kMaxDynActorVerts-1) for dynamic actor BLASes
  // (seated driver, Spectators.zip festival crowds, and Airborne Showcase aircraft).
  {
    std::vector<uint32_t> seq(kMaxDynActorVerts);
    for (uint32_t i = 0; i < kMaxDynActorVerts; ++i) seq[i] = i;
    const VkBufferUsageFlags rt_flags =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!r_.upload_buffer(seq.data(), seq.size() * sizeof(uint32_t), rt_flags, seq_ib_)) {
      error = "RT dynamic actor sequential index buffer";
      shutdown();
      return false;
    }
    seq_ib_bda_ = r_.buffer_device_address(seq_ib_.buffer);
  }

  // Size static world BLAS LRU budget to device VRAM (48 MiB on < 8 GiB GPUs, 160 MiB on 10+ GiB).
  const uint64_t vram_bytes = r_.rt_caps().device_local_bytes;
  world_lru_.set_budget(vram_bytes >= (10ull << 30) ? (160ull << 20) : (64ull << 20));

  ready_ = true;
  return true;
}

void RtScene::shutdown() {
  if (!r_.device_) return;
  VkDevice d = r_.device_;
  r_.wait_idle();

  for (auto& [k, entry] : world_blases_) destroy_blas(entry.blas, false);
  world_blases_.clear();
  world_lru_.clear();
  active_world_keys_.clear();
  pending_compactions_.clear();

  destroy_blas(tree_blas_, false);
  r_.destroy_buffer(tree_vb_);
  r_.destroy_buffer(tree_ib_);
  destroy_blas(spectator_blas_, false);
  if (spec_vb_mapped_) vmaUnmapMemory(r_.allocator_, spec_vb_.allocation);
  spec_vb_mapped_ = nullptr;
  spec_vb_cap_ = 0;
  r_.destroy_buffer(spec_vb_);
  r_.destroy_buffer(seq_ib_);
  seq_ib_bda_ = 0;
  registered_spectator_view_ = VK_NULL_HANDLE;
  registered_showcase_view_ = VK_NULL_HANDLE;

  for (auto& [car, set] : car_blases_) {
    destroy_blas(set.body, false);
    destroy_blas(set.glass, false);
    for (auto& wp : set.wheel_parts) destroy_blas(wp.blas, false);
    set.wheel_parts.clear();
  }
  car_blases_.clear();

  for (FrameSlot& s : slots_) {
    destroy_blas(s.driver_blas, false);
    destroy_blas(s.spectator_blas, false);
    destroy_blas(s.showcase_blas, false);
    if (s.dyn_vb_mapped) vmaUnmapMemory(r_.allocator_, s.dyn_vb.allocation);
    s.dyn_vb_mapped = nullptr;
    s.dyn_vb_cap = 0;
    r_.destroy_buffer(s.dyn_vb);

    if (s.tlas) vkDestroyAccelerationStructureKHR(d, s.tlas, nullptr);
    s.tlas = VK_NULL_HANDLE;
    r_.destroy_buffer(s.tlas_buf);
    r_.destroy_buffer(s.tlas_scratch);
    if (s.inst_vk_mapped) vmaUnmapMemory(r_.allocator_, s.inst_vk_buf.allocation);
    if (s.inst_info_mapped) vmaUnmapMemory(r_.allocator_, s.inst_info_buf.allocation);
    if (s.geom_info_mapped) vmaUnmapMemory(r_.allocator_, s.geom_info_buf.allocation);
    if (s.light_mapped) vmaUnmapMemory(r_.allocator_, s.light_buf.allocation);
    s.inst_vk_mapped = s.inst_info_mapped = s.geom_info_mapped = s.light_mapped = nullptr;
    r_.destroy_buffer(s.inst_vk_buf);
    r_.destroy_buffer(s.inst_info_buf);
    r_.destroy_buffer(s.geom_info_buf);
    r_.destroy_buffer(s.light_buf);
    s.geom_buffers.clear();
  }
  for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) {
    r_.destroy_buffer(blas_scratch_[i]);
    blas_scratch_bda_[i] = 0;
  }
  if (compact_pool_) {
    vkDestroyQueryPool(d, compact_pool_, nullptr);
    compact_pool_ = VK_NULL_HANDLE;
  }
  ready_ = false;
}

void RtScene::destroy_blas(BlasHandle& b, bool defer) {
  if (!b.handle && !b.buffer.buffer) return;
  VkAccelerationStructureKHR handle = b.handle;
  Renderer::Buffer buf = b.buffer;
  b.handle = VK_NULL_HANDLE;
  b.buffer = {};
  b.address = 0;
  b.bytes = 0;
  b.uncompacted_bytes = 0;
  b.compacted = false;
  b.compaction_pending = false;
  b.geoms.clear();
  b.tex_refs.clear();
  b.any_tex_pending = false;

  if (defer && retire_fn_) {
    Renderer* rp = &r_;
    retire_fn_([rp, handle, buf]() mutable {
      if (handle) vkDestroyAccelerationStructureKHR(rp->device_, handle, nullptr);
      rp->destroy_buffer(buf);
    });
  } else {
    if (handle) vkDestroyAccelerationStructureKHR(r_.device_, handle, nullptr);
    r_.destroy_buffer(buf);
  }
}

void RtScene::invalidate_car(const Showroom* car) {
  auto it = car_blases_.find(car);
  if (it == car_blases_.end()) return;
  destroy_blas(it->second.body, true);
  destroy_blas(it->second.glass, true);
  for (auto& wp : it->second.wheel_parts) destroy_blas(wp.blas, true);
  it->second.wheel_parts.clear();
  car_blases_.erase(it);
}

void RtScene::invalidate_world() {
  for (auto& [k, e] : world_blases_) destroy_blas(e.blas, true);
  world_blases_.clear();
  world_lru_.clear();
  active_world_keys_.clear();
  last_world_blas_gen_ = ~0ull;
  last_world_draw_count_ = 0;
  last_world_vb_ = VK_NULL_HANDLE;
  last_world_ib_ = VK_NULL_HANDLE;
  last_world_blases_complete_ = false;
  pending_compactions_.clear();
  destroy_blas(tree_blas_, true);
  tree_world_gen_ = ~0ull;
  tree_textures_pending_ = false;
  tree_tex_count_ = 0;
}

bool RtScene::ensure_scratch(VkCommandBuffer cmd, VkDeviceSize needed, VkDeviceAddress& out_addr) {
  uint64_t off = scratch_alloc_.allocate(uint64_t(needed));
  if (off == UINT64_MAX) {
    // Scratch ring full on a burst frame (e.g. initial scene load): synchronize prior BLAS
    // builds and safely recycle the scratch buffer from offset 0 so zero BLASes are ever dropped!
    as_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
               VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    scratch_alloc_.begin_frame();
    off = scratch_alloc_.allocate(uint64_t(needed));
    if (off == UINT64_MAX) return false;
  }
  out_addr = blas_scratch_bda_[active_slot_] + off;
  return true;
}

bool RtScene::build_blas_from_geoms(VkCommandBuffer cmd,
                                    const std::vector<VkAccelerationStructureGeometryKHR>& vk_geoms,
                                    const std::vector<uint32_t>& prim_counts,
                                    const std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges,
                                    bool allow_compaction, BlasHandle& out, uint32_t* out_query_index) {
  if (vk_geoms.empty()) return false;
  VkDevice d = r_.device_;

  VkAccelerationStructureBuildGeometryInfoKHR build_info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     (allow_compaction ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0u);
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = uint32_t(vk_geoms.size());
  build_info.pGeometries = vk_geoms.data();

  VkAccelerationStructureBuildSizesInfoKHR size_info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info,
                                          prim_counts.data(), &size_info);

  VkDeviceAddress scratch_bda = 0;
  if (!ensure_scratch(cmd, size_info.buildScratchSize, scratch_bda)) return false;

  if (!r_.create_buffer(size_info.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.buffer)) {
    return false;
  }

  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = out.buffer.buffer;
  ci.size = size_info.accelerationStructureSize;
  ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  if (vkCreateAccelerationStructureKHR(d, &ci, nullptr, &out.handle) != VK_SUCCESS) {
    r_.destroy_buffer(out.buffer);
    return false;
  }

  build_info.dstAccelerationStructure = out.handle;
  build_info.scratchData.deviceAddress = scratch_bda;
  const VkAccelerationStructureBuildRangeInfoKHR* p_ranges = ranges.data();
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build_info, &p_ranges);

  VkAccelerationStructureDeviceAddressInfoKHR addr_info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  addr_info.accelerationStructure = out.handle;
  out.address = vkGetAccelerationStructureDeviceAddressKHR(d, &addr_info);
  out.uncompacted_bytes = size_info.accelerationStructureSize;
  out.bytes = size_info.accelerationStructureSize;
  out.compacted = false;

  if (allow_compaction && out_query_index && compact_pool_ &&
      pending_compactions_.size() + 16u < kMaxCompactionQueries) {
    as_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
               VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const uint32_t qidx = next_query_ % kMaxCompactionQueries;
    ++next_query_;
    vkCmdResetQueryPool(cmd, compact_pool_, qidx, 1);
    vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &out.handle,
                                                  VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                  compact_pool_, qidx);
    out.compaction_pending = true;
    *out_query_index = qidx;
  }
  return true;
}

void RtScene::process_compactions(VkCommandBuffer cmd) {
  if (!compact_pool_ || pending_compactions_.empty()) return;
  const uint64_t done = r_.frames_done();
  VkDevice d = r_.device_;

  uint32_t compacted_this_frame = 0;
  constexpr uint32_t kMaxCompactionsPerFrame = 8;

  while (!pending_compactions_.empty() && compacted_this_frame < kMaxCompactionsPerFrame) {
    const PendingCompaction& pc = pending_compactions_.front();
    if (pc.submit_tag > done) break;

    uint64_t compact_bytes = 0;
    const VkResult qr = vkGetQueryPoolResults(d, compact_pool_, pc.query_index, 1, sizeof(compact_bytes),
                                              &compact_bytes, sizeof(compact_bytes), VK_QUERY_RESULT_64_BIT);
    if (qr == VK_NOT_READY) break;

    const PendingCompaction item = pc;
    pending_compactions_.pop_front();

    if (qr != VK_SUCCESS || compact_bytes == 0) continue;
    auto it = world_blases_.find(item.world_key);
    if (it == world_blases_.end()) continue;
    BlasHandle& bh = it->second.blas;
    if (!bh.handle || bh.compacted) continue;
    bh.compaction_pending = false;

    if (compact_bytes + 4096 >= bh.bytes) {
      bh.compacted = true;
      continue;
    }

    Renderer::Buffer new_buf;
    if (!r_.create_buffer(compact_bytes,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, new_buf)) {
      continue;
    }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = new_buf.buffer;
    ci.size = compact_bytes;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR new_as = VK_NULL_HANDLE;
    if (vkCreateAccelerationStructureKHR(d, &ci, nullptr, &new_as) != VK_SUCCESS) {
      r_.destroy_buffer(new_buf);
      continue;
    }

    VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
    copy.src = bh.handle;
    copy.dst = new_as;
    copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    vkCmdCopyAccelerationStructureKHR(cmd, &copy);

    VkAccelerationStructureDeviceAddressInfoKHR ai{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = new_as;
    const VkDeviceAddress new_addr = vkGetAccelerationStructureDeviceAddressKHR(d, &ai);

    // Retire old uncompacted BLAS safely after frames in flight finish.
    VkAccelerationStructureKHR old_as = bh.handle;
    Renderer::Buffer old_buf = bh.buffer;
    if (retire_fn_) {
      Renderer* rp = &r_;
      retire_fn_([rp, old_as, old_buf]() mutable {
        if (old_as) vkDestroyAccelerationStructureKHR(rp->device_, old_as, nullptr);
        rp->destroy_buffer(old_buf);
      });
    }

    bh.handle = new_as;
    bh.buffer = new_buf;
    bh.address = new_addr;
    bh.bytes = compact_bytes;
    bh.compacted = true;
    world_lru_.mark_compacted(item.world_key, compact_bytes);
    ++compacted_this_frame;
  }
}

bool RtScene::ensure_car_blas(VkCommandBuffer cmd, const Showroom& car, CarBlasSet& out) {
  if (!car.vertex_buffer_.buffer || !car.index_buffer_.buffer || car.vertices_.empty() || car.draws_.empty())
    return false;
  if (out.vb_id == car.vertex_buffer_.buffer && out.ib_id == car.index_buffer_.buffer && out.body.handle)
    return true;

  // Rebuild modular sub-BLASes if this car's buffers changed (e.g. hot-swap).
  destroy_blas(out.body, true);
  destroy_blas(out.glass, true);
  out.body_draw = -1;
  out.glass_draw = -1;
  out.use_far = car.guest_ && !car.far_draws_.empty();
  for (auto& wp : out.wheel_parts) destroy_blas(wp.blas, true);
  out.wheel_parts.clear();

  const auto& src_draws = out.use_far ? car.far_draws_ : car.draws_;
  const VkDeviceAddress vb_bda = r_.buffer_device_address(car.vertex_buffer_.buffer);
  const VkDeviceAddress ib_bda = r_.buffer_device_address(car.index_buffer_.buffer);
  if (!vb_bda || !ib_bda) return false;
  const uint32_t total_verts = uint32_t(car.vertices_.size());

  auto build_group = [&](const std::vector<int>& draw_indices, bool is_glass, BlasHandle& target,
                         std::vector<uint8_t>* out_lamp_channels = nullptr) {
    if (out_lamp_channels) out_lamp_channels->clear();
    if (draw_indices.empty()) return;
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<uint32_t> prims;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<RtGeomInfoGpu> meta;
    std::vector<uint8_t> lamps;
    geoms.reserve(draw_indices.size());
    prims.reserve(draw_indices.size());
    ranges.reserve(draw_indices.size());
    meta.reserve(draw_indices.size());
    lamps.reserve(draw_indices.size());

    for (int idx : draw_indices) {
      const Showroom::Draw& d = src_draws[size_t(idx)];
      if (d.index_count < 3 || d.vertex_offset < 0 || uint32_t(d.vertex_offset) >= total_verts) continue;
      const uint32_t prim_count = d.index_count / 3u;
      const VkDeviceAddress g_vb = vb_bda + uint64_t(d.vertex_offset) * sizeof(cars::bake::Vertex);
      const VkDeviceAddress g_ib = ib_bda + uint64_t(d.first_index) * sizeof(uint32_t);

      VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      ag.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      ag.flags = is_glass ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
      auto& tri = ag.geometry.triangles;
      tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      tri.vertexData.deviceAddress = g_vb;
      tri.vertexStride = sizeof(cars::bake::Vertex);
      tri.maxVertex = total_verts - uint32_t(d.vertex_offset) - 1u;
      tri.indexType = VK_INDEX_TYPE_UINT32;
      tri.indexData.deviceAddress = g_ib;
      geoms.push_back(ag);
      prims.push_back(prim_count);
      ranges.push_back({prim_count, 0, 0, 0});

      RtGeomInfoGpu gm{};
      gm.vb_off = uint32_t(d.vertex_offset);
      gm.ib_off = d.first_index;
      fill_car_geom_material_raw(d.push, d.lamp_channel, d.brake_lamp, car.frame().paint_finish[0], gm);
      meta.push_back(gm);
      lamps.push_back(uint8_t(d.lamp_channel));
    }
    if (!geoms.empty()) {
      if (build_blas_from_geoms(cmd, geoms, prims, ranges, false, target, nullptr)) {
        target.geoms = std::move(meta);
        target.vb_buf = car.vertex_buffer_.buffer;
        target.ib_buf = car.index_buffer_.buffer;
        if (out_lamp_channels) *out_lamp_channels = std::move(lamps);
      }
    }
  };

  std::vector<int> body_draws, glass_draws;
  struct WheelGroup {
    int wheel = -1;
    bool spins = false;
    float local[16] = {};
    int rep_draw = -1;
    std::vector<int> draws;
  };
  std::vector<WheelGroup> wheel_groups;

  for (int i = 0; i < int(src_draws.size()); ++i) {
    const Showroom::Draw& d = src_draws[size_t(i)];
    if (d.index_count < 3) continue;
    const uint32_t klass = uint32_t(d.push.params[0] + 0.5f);
    if (klass == 11u) continue;  // kHidden
    // Guest/opponent cars skip interior, gauge, and caliper geometry in RT BLASes.
    if (car.guest_ && (klass == 4u || klass == 5u || klass == 8u)) continue;
    if (d.wheel >= 0 && d.wheel < 4) {
      // Group wheel draws that share the exact same (wheel, spins, local matrix):
      // rim, tyre, rotor, and caliper each have their own distinct `d.local`.
      WheelGroup* match = nullptr;
      for (auto& wg : wheel_groups) {
        if (wg.wheel == d.wheel && wg.spins == d.spins) {
          bool same = true;
          for (int k = 0; k < 16; ++k) {
            if (std::fabs(wg.local[k] - d.local[k]) > 1e-5f) {
              same = false;
              break;
            }
          }
          if (same) {
            match = &wg;
            break;
          }
        }
      }
      if (!match) {
        WheelGroup wg;
        wg.wheel = d.wheel;
        wg.spins = d.spins;
        std::memcpy(wg.local, d.local, sizeof(wg.local));
        wg.rep_draw = i;
        wheel_groups.push_back(std::move(wg));
        match = &wheel_groups.back();
      }
      match->draws.push_back(i);
    } else if (d.glass) {
      glass_draws.push_back(i);
      if (out.glass_draw < 0) out.glass_draw = i;
    } else {
      body_draws.push_back(i);
      if (out.body_draw < 0) out.body_draw = i;
    }
  }

  build_group(body_draws, false, out.body, &out.body_lamp_channels);
  build_group(glass_draws, true, out.glass);
  out.wheel_parts.reserve(wheel_groups.size());
  for (const auto& wg : wheel_groups) {
    WheelSubBlas sub;
    sub.draw_idx = wg.rep_draw;
    build_group(wg.draws, false, sub.blas);
    if (sub.blas.handle) out.wheel_parts.push_back(std::move(sub));
  }
  out.vb_id = car.vertex_buffer_.buffer;
  out.ib_id = car.index_buffer_.buffer;
  return out.body.handle != VK_NULL_HANDLE;
}

void RtScene::update_world_blases(VkCommandBuffer cmd, const Showroom& host) {
  const bool arena = host.arena_on() && host.gpu_world_ != nullptr;
  VkBuffer vb = arena ? host.gpu_world_->vertex_buffer() : host.world_vertices_.buffer;
  VkBuffer ib = arena ? host.gpu_world_->index_buffer() : host.world_indices_.buffer;
  if (!vb || !ib || host.world_draws_.empty()) return;

  // Steady-state O(1) fast path: when world geometry generation, draw count, and
  // backing buffers haven't changed and all cells are built, skip regrouping/hashing
  // thousands of draws every frame and keep existing active_world_keys_ live.
  if (last_world_blases_complete_ && host.world_generation_ == last_world_blas_gen_ &&
      host.world_draws_.size() == last_world_draw_count_ && vb == last_world_vb_ && ib == last_world_ib_) {
    for (uint64_t k : active_world_keys_) {
      auto it = world_blases_.find(k);
      if (it != world_blases_.end()) it->second.last_frame = frame_counter_;
    }
    const auto evicted = world_lru_.evict(r_.frames_done(), false);
    for (uint64_t ek : evicted) {
      auto it = world_blases_.find(ek);
      if (it != world_blases_.end()) {
        destroy_blas(it->second.blas, true);
        world_blases_.erase(it);
      }
    }
    return;
  }

  const VkDeviceAddress vb_bda = r_.buffer_device_address(vb);
  const VkDeviceAddress ib_bda = r_.buffer_device_address(ib);
  if (!vb_bda || !ib_bda) return;

  // Release previous frame's active world key references before re-acquiring current frame's set.
  const uint64_t submit_tag = r_.frames_submitted();
  for (uint64_t old_k : active_world_keys_) world_lru_.release(old_k, submit_tag);
  active_world_keys_.clear();

  // Group current world draws by (quantized origin, 128m spatial cell, opacity class).
  struct CellGroup {
    int32_t cell_x = 0, cell_z = 0;
    double origin_x = 0.0, origin_z = 0.0;
    uint8_t opacity_class = 0;
    uint64_t content_hash = 0x14650FB0739D0383ull;
    std::vector<const WorldGeometry::Draw*> draws;
  };
  struct GroupKey {
    int64_t ox_mm = 0, oz_mm = 0;
    int32_t cx = 0, cz = 0;
    uint8_t op = 0;
    bool operator==(const GroupKey& o) const {
      return ox_mm == o.ox_mm && oz_mm == o.oz_mm && cx == o.cx && cz == o.cz && op == o.op;
    }
  };
  struct GroupKeyHash {
    size_t operator()(const GroupKey& k) const {
      uint64_t h = uint64_t(k.ox_mm) * 0x9E3779B97F4A7C15ull ^ uint64_t(k.oz_mm);
      h = (h * 0xBF58476D1CE4E5B9ull) ^ (uint64_t(uint32_t(k.cx)) << 32) ^ uint64_t(uint32_t(k.cz));
      h ^= uint64_t(k.op) * 0x94D049BB133111EBull;
      return size_t(h);
    }
  };

  std::unordered_map<GroupKey, CellGroup, GroupKeyHash> groups;
  const double wox = host.world_origin_[0];
  const double woz = host.world_origin_[1];
  const uint32_t mono_total_verts =
      arena ? host.gpu_world_->resident().vertex_capacity() : uint32_t(host.world_vertices_.size / sizeof(WorldVertex));

  for (const WorldGeometry::Draw& d : host.world_draws_) {
    if (d.index_count < 3) continue;
    const uint32_t wflags = uint32_t(std::lround(d.push.params[1]));
    if ((wflags & gpuworld::kFlagSky) != 0u) continue;  // analytical/TOD sky handles misses
    if ((wflags & 32u) != 0u) continue;                  // legacy billboard trees (handled by cross-quad BLAS)
    if ((wflags & 256u) != 0u) continue;                 // far-LOD10 duplicate props (avoid doubling over LOD00)
    if (d.push.params[2] > 0.5f) continue;               // far-ring depth-pushed backdrop shell (causes near terrain overlap)
    if (d.blend == 3u) continue;                         // skip additive glow cards from BVH
    const bool is_road_decal = (d.decal != 0u && d.blend >= 2u);
    if (!is_road_decal && d.blend >= 2u && (wflags & 32768u) == 0u) continue;  // skip non-decal alpha overlays

    double draw_ox = wox, draw_oz = woz;
    if (arena && d.resident) {
      const auto* rd = host.gpu_world_->resident().find(d.key);
      if (!rd || !rd->valid) continue;
      draw_ox = rd->origin_x;
      draw_oz = rd->origin_z;
    }
    const double mid_x = draw_ox + 0.5 * double(d.box_min[0] + d.box_max[0]);
    const double mid_z = draw_oz + 0.5 * double(d.box_min[2] + d.box_max[2]);
    const int32_t cx = int32_t(std::floor(mid_x / 128.0));
    const int32_t cz = int32_t(std::floor(mid_z / 128.0));
    // opacity_class: 0=opaque, 1=cutout/foliage, 2=water, 3=road decals (lifted +1.2cm, non-opaque alpha test)
    const uint8_t op = ((wflags & 32768u) != 0u) ? 2u : (is_road_decal ? 3u : (d.blend == 1u ? 1u : 0u));

    GroupKey gk{gpuworld::quantize_mm(draw_ox), gpuworld::quantize_mm(draw_oz), cx, cz, op};
    CellGroup& cg = groups[gk];
    if (cg.draws.empty()) {
      cg.cell_x = cx;
      cg.cell_z = cz;
      cg.origin_x = draw_ox;
      cg.origin_z = draw_oz;
      cg.opacity_class = op;
    }
    const uint64_t dk = d.key ? d.key : (uint64_t(d.first_index) << 32 | uint64_t(d.index_count));
    cg.content_hash = gpuworld::hash_mix(cg.content_hash, dk);
    cg.draws.push_back(&d);
  }

  // On initial frames, allow full burst build so first-frame / headless captures have complete BVH;
  // during steady-state driving, throttle new BLAS builds per frame for hitch-free streaming.
  const uint32_t build_budget = (frame_counter_ < 4u) ? 512u : 8u;
  uint32_t built_this_frame = 0;
  bool any_skipped_for_budget = false;

  for (auto& [gk, cg] : groups) {
    const uint64_t bkey = rt::make_world_cell_key(cg.cell_x, cg.cell_z, cg.opacity_class, cg.content_hash);
    if (world_lru_.acquire(bkey, frame_counter_)) {
      auto it = world_blases_.find(bkey);
      if (it != world_blases_.end()) {
        it->second.last_frame = frame_counter_;
        active_world_keys_.push_back(bkey);
      }
      continue;
    }
    if (built_this_frame >= build_budget) {
      any_skipped_for_budget = true;
      continue;
    }

    std::vector<VkAccelerationStructureGeometryKHR> vk_geoms;
    std::vector<uint32_t> prims;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<RtGeomInfoGpu> meta;
    std::vector<WorldGeomTexRecords> tex_refs;
    bool any_pending = false;
    vk_geoms.reserve(cg.draws.size());
    prims.reserve(cg.draws.size());
    ranges.reserve(cg.draws.size());
    meta.reserve(cg.draws.size());
    tex_refs.reserve(cg.draws.size());

    for (const WorldGeometry::Draw* dp : cg.draws) {
      const WorldGeometry::Draw& d = *dp;
      uint32_t vbase = 0, vcount = 0, ibase = 0, icount = 0;
      if (arena && d.resident) {
        const auto* rd = host.gpu_world_->resident().find(d.key);
        if (!rd || !rd->valid || rd->vertex_count == 0 || rd->index_count < 3) continue;
        vbase = rd->vertex_base;
        vcount = rd->vertex_count;
        ibase = rd->index_base;
        icount = rd->index_count;
      } else {
        if (d.vertex_offset < 0 || uint32_t(d.vertex_offset) >= mono_total_verts || d.index_count < 3) continue;
        vbase = uint32_t(d.vertex_offset);
        vcount = mono_total_verts - vbase;
        ibase = d.first_index;
        icount = d.index_count;
      }
      const uint32_t prim_count = icount / 3u;
      const VkDeviceAddress g_vb = vb_bda + uint64_t(vbase) * sizeof(WorldVertex);
      const VkDeviceAddress g_ib = ib_bda + uint64_t(ibase) * sizeof(uint32_t);

      VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      ag.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      ag.flags = (cg.opacity_class == 0u || cg.opacity_class == 2u)
                     ? VK_GEOMETRY_OPAQUE_BIT_KHR
                     : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
      auto& tri = ag.geometry.triangles;
      tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      tri.vertexData.deviceAddress = g_vb;
      tri.vertexStride = sizeof(WorldVertex);
      tri.maxVertex = vcount - 1u;
      tri.indexType = VK_INDEX_TYPE_UINT32;
      tri.indexData.deviceAddress = g_ib;
      vk_geoms.push_back(ag);
      prims.push_back(prim_count);
      ranges.push_back({prim_count, 0, 0, 0});

      RtGeomInfoGpu gm{};
      gm.vb_off = vbase;
      gm.ib_off = ibase;
      WorldGeomTexRecordsLocal tr_loc{};
      fill_world_geom_material(d, host.gpu_world_.get(), gm, tr_loc);
      meta.push_back(gm);
      WorldGeomTexRecords tr{};
      tr.albedo = tr_loc.albedo;
      tr.aux = tr_loc.aux;
      tr.splat = tr_loc.splat;
      tr.layer_c = tr_loc.layer_c;
      tr.ao = tr_loc.ao;
      tr.pending = tr_loc.pending;
      if (tr.pending) any_pending = true;
      tex_refs.push_back(tr);
    }

    if (vk_geoms.empty()) continue;
    WorldCellEntry entry{};
    entry.origin_x = cg.origin_x;
    entry.origin_z = cg.origin_z;
    entry.opacity_class = cg.opacity_class;
    entry.last_frame = frame_counter_;

    uint32_t qidx = 0;
    if (build_blas_from_geoms(cmd, vk_geoms, prims, ranges, true, entry.blas, &qidx)) {
      entry.blas.geoms = std::move(meta);
      entry.blas.tex_refs = std::move(tex_refs);
      entry.blas.vb_buf = vb;
      entry.blas.ib_buf = ib;
      entry.blas.any_tex_pending = any_pending;
      world_lru_.insert(bkey, entry.blas.bytes, frame_counter_);
      if (entry.blas.compaction_pending) {
        pending_compactions_.push_back({bkey, qidx, submit_tag + 1u});
      }
      world_blases_.emplace(bkey, std::move(entry));
      active_world_keys_.push_back(bkey);
      ++built_this_frame;
    }
  }

  // Evict LRU unreferenced world BLASes once GPU frames using them have completed.
  const auto evicted = world_lru_.evict(r_.frames_done(), false);
  for (uint64_t ek : evicted) {
    auto it = world_blases_.find(ek);
    if (it != world_blases_.end()) {
      destroy_blas(it->second.blas, true);
      world_blases_.erase(it);
    }
  }
  last_world_blas_gen_ = host.world_generation_;
  last_world_draw_count_ = host.world_draws_.size();
  last_world_vb_ = vb;
  last_world_ib_ = ib;
  last_world_blases_complete_ = !any_skipped_for_budget;
}

void RtScene::update_tree_blas(VkCommandBuffer cmd, const Showroom& host) {
  if (host.world_trees_.empty()) return;
  const size_t cur_tex_count = host.gpu_world_ ? host.gpu_world_->texture_count() : 0u;
  if (host.world_generation_ == tree_world_gen_ && (!tree_textures_pending_ || cur_tex_count == tree_tex_count_)) {
    return;
  }
  tree_world_gen_ = host.world_generation_;
  tree_tex_count_ = cur_tex_count;
  tree_textures_pending_ = false;
  tree_origin_x_ = host.world_origin_[0];
  tree_origin_z_ = host.world_origin_[1];

  // Expand nearby Colorado pine tree sprites into 2-quad orthogonal cross-cards so
  // reflections, shadows, and path-traced bounces see 3D foliage from any angle.
  // Never bake kDefaultWhite (slot 0) onto uncached trees; defer until WorldGpu uploads them.
  const size_t max_trees = std::min<size_t>(host.world_trees_.size(), 49152u);
  std::vector<WorldVertex> verts;
  std::vector<uint32_t> idxs;
  verts.reserve(max_trees * 8u);
  idxs.reserve(max_trees * 12u);

  const uint32_t step = uint32_t(std::max<size_t>(1u, host.world_trees_.size() / max_trees));
  uint32_t first_tex_slot = 0;
  for (size_t ti = 0; ti < host.world_trees_.size() && (verts.size() / 8u) < max_trees; ti += step) {
    const auto& tr = host.world_trees_[ti];
    if (!host.gpu_world_ || tr.texture == kNoWorldTexture) continue;
    if (!host.gpu_world_->has_texture(tr.texture)) {
      tree_textures_pending_ = true;
      continue;
    }
    const uint32_t tree_slot = host.gpu_world_->texture_slot(tr.texture);
    if (tree_slot < 3u) {
      tree_textures_pending_ = true;
      continue;
    }
    if (first_tex_slot == 0) first_tex_slot = tree_slot;
    const float hw = std::max(tr.half_width, 0.5f);
    const float h = std::max(tr.height, 1.0f);
    // Deterministic azimuth hash from tree base XZ.
    const float az = std::fmod(std::fabs(tr.base[0] * 12.9898f + tr.base[2] * 78.233f), 3.14159265f);
    const float ca = std::cos(az), sa = std::sin(az);
    const float axes[2][2] = {{ca, sa}, {-sa, ca}};
    const uint8_t cr = uint8_t(tr.rgba8 & 0xFFu);
    const uint8_t cg = uint8_t((tr.rgba8 >> 8) & 0xFFu);
    const uint8_t cb = uint8_t((tr.rgba8 >> 16) & 0xFFu);

    for (int q = 0; q < 2; ++q) {
      const uint32_t base_v = uint32_t(verts.size());
      const float rx = axes[q][0] * hw;
      const float rz = axes[q][1] * hw;
      const float nx = -axes[q][1], nz = axes[q][0];
      const int8_t inx = int8_t(std::clamp(nx * 127.0f, -127.0f, 127.0f));
      const int8_t inz = int8_t(std::clamp(nz * 127.0f, -127.0f, 127.0f));
      const float xs[4] = {-rx, rx, rx, -rx};
      const float zs[4] = {-rz, rz, rz, -rz};
      const float ys[4] = {0.0f, 0.0f, h, h};
      const float us[4] = {tr.uv[0], tr.uv[2], tr.uv[2], tr.uv[0]};
      const float vs[4] = {tr.uv[3], tr.uv[3], tr.uv[1], tr.uv[1]};
      for (int c = 0; c < 4; ++c) {
        WorldVertex v{};
        v.position[0] = tr.base[0] + xs[c];
        v.position[1] = tr.base[1] + ys[c];
        v.position[2] = tr.base[2] + zs[c];
        v.normal[0] = inx;
        v.normal[1] = 48;
        v.normal[2] = inz;
        v.normal[3] = 0;
        v.uv0[0] = us[c];
        v.uv0[1] = vs[c];
        v.colour[0] = cr;
        v.colour[1] = cg;
        v.colour[2] = cb;
        v.colour[3] = 255;
        v.uv2[0] = uint16_t(tree_slot & 0xFFFFu);
        verts.push_back(v);
      }
      idxs.push_back(base_v + 0); idxs.push_back(base_v + 1); idxs.push_back(base_v + 2);
      idxs.push_back(base_v + 0); idxs.push_back(base_v + 2); idxs.push_back(base_v + 3);
    }
  }
  if (verts.empty() || idxs.empty()) return;

  // Retire old tree buffers & BLAS cleanly.
  destroy_blas(tree_blas_, true);
  if (tree_vb_.buffer || tree_ib_.buffer) {
    Renderer* rp = &r_;
    Renderer::Buffer old_vb = tree_vb_, old_ib = tree_ib_;
    tree_vb_ = {};
    tree_ib_ = {};
    if (retire_fn_) {
      retire_fn_([rp, old_vb, old_ib]() mutable {
        rp->destroy_buffer(old_vb);
        rp->destroy_buffer(old_ib);
      });
    } else {
      r_.destroy_buffer(old_vb);
      r_.destroy_buffer(old_ib);
    }
  }

  const VkBufferUsageFlags rt_flags =
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!r_.upload_buffer(verts.data(), verts.size() * sizeof(WorldVertex), rt_flags, tree_vb_) ||
      !r_.upload_buffer(idxs.data(), idxs.size() * sizeof(uint32_t), rt_flags, tree_ib_)) {
    return;
  }

  const VkDeviceAddress tvb = r_.buffer_device_address(tree_vb_.buffer);
  const VkDeviceAddress tib = r_.buffer_device_address(tree_ib_.buffer);
  const uint32_t prim_count = uint32_t(idxs.size() / 3u);

  VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  ag.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  ag.flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
  auto& tri = ag.geometry.triangles;
  tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  tri.vertexData.deviceAddress = tvb;
  tri.vertexStride = sizeof(WorldVertex);
  tri.maxVertex = uint32_t(verts.size() - 1u);
  tri.indexType = VK_INDEX_TYPE_UINT32;
  tri.indexData.deviceAddress = tib;

  if (build_blas_from_geoms(cmd, {ag}, {prim_count}, {{prim_count, 0, 0, 0}}, false, tree_blas_, nullptr)) {
    RtGeomInfoGpu gm{};
    gm.vb_off = 0u;
    gm.ib_off = 0u;
    gm.material_class = 103u;
    gm.flags = kRtGeomFlagCutout | kRtGeomFlagTreeCross | (first_tex_slot ? kRtGeomFlagBindlessTex : 0u);
    gm.tex_albedo = first_tex_slot;
    gm.uv0_xform[0] = 1.0f; gm.uv0_xform[1] = 1.0f; gm.uv0_xform[2] = 1.0f; gm.uv0_xform[3] = 1.0f;
    gm.tint[0] = 0.16f;
    gm.tint[1] = 0.24f;
    gm.tint[2] = 0.12f;
    gm.tint[3] = 0.88f;
    tree_blas_.geoms = {gm};
    tree_blas_.vb_buf = tree_vb_.buffer;
    tree_blas_.ib_buf = tree_ib_.buffer;
  }
}

void RtScene::update_dynamic_actor_blases(VkCommandBuffer cmd, const Showroom& host, FrameSlot& slot) {
  // Destroy this frame-slot's previous dynamic actor BLASes (safe: frame fence already signaled).
  destroy_blas(slot.driver_blas, false);
  destroy_blas(slot.spectator_blas, false);
  destroy_blas(slot.showcase_blas, false);
  if (!seq_ib_bda_) return;

  // 1. Collect seated driver vertices (car model space).
  const uint32_t drv_n =
      (host.vfx_ready_ && host.driver_buf_.ready() && host.driver_count_ >= 3u)
          ? (std::min(host.driver_count_, kMaxDynActorVerts) / 3u) * 3u
          : 0u;

  // 2. Throttled Spectators.zip festival crowd BLAS (rebuilt every 6 frames or when world origin/eye shifts > 4 m).
  if (host.vfx_ready_ && host.spectator_anim_.catalog().has_3d_spectators()) {
    const auto eye = host.eye_in_scenery(host.world_transform_);
    const double eye_gx = host.world_origin_[0] + double(eye[0]);
    const double eye_gy = double(eye[1]);
    const double eye_gz = host.world_origin_[1] + double(eye[2]);
    const bool origin_changed = (host.world_origin_[0] != spectator_last_origin_x_ ||
                                 host.world_origin_[1] != spectator_last_origin_z_);
    const double ed2 = (eye_gx - spectator_last_eye_x_) * (eye_gx - spectator_last_eye_x_) +
                       (eye_gz - spectator_last_eye_z_) * (eye_gz - spectator_last_eye_z_);
    const bool need_spec_rebuild = !spectator_blas_.handle || origin_changed || ed2 > 16.0 ||
                                   (frame_counter_ - spectator_last_frame_) >= 6ull;
    if (need_spec_rebuild) {
      spectator_last_frame_ = frame_counter_;
      spectator_last_origin_x_ = host.world_origin_[0];
      spectator_last_origin_z_ = host.world_origin_[1];
      spectator_last_eye_x_ = eye_gx;
      spectator_last_eye_z_ = eye_gz;
      const drive::Ground* active_ground =
          host.world_ground_ ? host.world_ground_ : world::AnimatedWorldSystem::last_ground();
      thread_local std::vector<world::SpectatorVertex> cpu_spec;
      cpu_spec.clear();
      host.spectator_anim_.build_spectator_mesh(eye_gx, eye_gy, eye_gz, host.tod_real_time_, active_ground, cpu_spec);
      const uint32_t sn = (uint32_t(std::min(cpu_spec.size(), size_t(kMaxDynActorVerts))) / 3u) * 3u;
      destroy_blas(spectator_blas_, true);
      if (sn >= 3u) {
        uint32_t spec_tex_slot = 0u;
        constexpr uint32_t kSpectatorTexRecord = 0xFFFFFFF0u;
        if (host.gpu_world_ && host.spectator_atlas_.view) {
          if (registered_spectator_view_ != host.spectator_atlas_.view ||
              !host.gpu_world_->has_texture(kSpectatorTexRecord)) {
            spec_tex_slot = host.gpu_world_->add_texture(kSpectatorTexRecord, host.spectator_atlas_.view);
            registered_spectator_view_ = host.spectator_atlas_.view;
          } else {
            spec_tex_slot = host.gpu_world_->texture_slot(kSpectatorTexRecord);
          }
        }
        if (!spec_vb_.buffer || spec_vb_cap_ < sn) {
          const uint32_t alloc_n = std::max(sn, std::max(spec_vb_cap_ * 2u, 8192u));
          if (spec_vb_mapped_) {
            vmaUnmapMemory(r_.allocator_, spec_vb_.allocation);
            spec_vb_mapped_ = nullptr;
          }
          if (spec_vb_.buffer) {
            Renderer::Buffer old_vb = spec_vb_;
            spec_vb_ = {};
            if (retire_fn_) {
              Renderer* rp = &r_;
              retire_fn_([rp, old_vb]() mutable { rp->destroy_buffer(old_vb); });
            } else {
              r_.destroy_buffer(old_vb);
            }
          }
          const VkBufferUsageFlags usage =
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
          if (r_.create_buffer(VkDeviceSize(alloc_n) * sizeof(WorldVertex), usage,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, spec_vb_) &&
              vmaMapMemory(r_.allocator_, spec_vb_.allocation, &spec_vb_mapped_) == VK_SUCCESS) {
            spec_vb_cap_ = alloc_n;
          } else {
            r_.destroy_buffer(spec_vb_);
            spec_vb_cap_ = 0;
            spec_vb_mapped_ = nullptr;
          }
        }
        if (spec_vb_.buffer && spec_vb_mapped_) {
          const auto& cat = host.spectator_anim_.catalog();
          const uint32_t asz = world::SpectatorCatalog::kAtlasSize;
          auto snorm8 = [](float x) -> int8_t {
            return int8_t(std::clamp(int(x * 127.0f + (x >= 0.0f ? 0.5f : -0.5f)), -127, 127));
          };
          auto* sdst = static_cast<WorldVertex*>(spec_vb_mapped_);
          const bool has_atlas = cat.atlas_rgba.size() >= size_t(asz) * asz * 4u;
          for (uint32_t i = 0; i < sn; ++i) {
            const auto& v = cpu_spec[i];
            WorldVertex& o = sdst[i];
            o = WorldVertex{};
            o.position[0] = float(double(v.pos[0]) - host.world_origin_[0]);
            o.position[1] = v.pos[1];
            o.position[2] = float(double(v.pos[2]) - host.world_origin_[1]);
            o.normal[0] = snorm8(v.normal[0]);
            o.normal[1] = snorm8(v.normal[1]);
            o.normal[2] = snorm8(v.normal[2]);
            o.uv0[0] = v.uv[0];
            o.uv0[1] = v.uv[1];
            if (has_atlas) {
              const uint32_t tx = uint32_t(std::clamp(v.uv[0], 0.0f, 0.9999f) * float(asz));
              const uint32_t ty = uint32_t(std::clamp(v.uv[1], 0.0f, 0.9999f) * float(asz));
              const uint8_t* px = cat.atlas_rgba.data() + (size_t(ty) * asz + tx) * 4u;
              o.colour[0] = px[0];
              o.colour[1] = px[1];
              o.colour[2] = px[2];
              o.colour[3] = 255;
            } else {
              for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
            }
          }
          const uint32_t prim_count = sn / 3u;
          VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
          ag.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
          ag.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
          auto& tri = ag.geometry.triangles;
          tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
          tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
          tri.vertexData.deviceAddress = r_.buffer_device_address(spec_vb_.buffer);
          tri.vertexStride = sizeof(WorldVertex);
          tri.maxVertex = sn - 1u;
          tri.indexType = VK_INDEX_TYPE_UINT32;
          tri.indexData.deviceAddress = seq_ib_bda_;
          if (build_blas_from_geoms(cmd, {ag}, {prim_count}, {{prim_count, 0, 0, 0}}, false, spectator_blas_, nullptr)) {
            RtGeomInfoGpu gm{};
            gm.vb_off = 0u;
            gm.ib_off = 0u;
            gm.material_class = 100u;
            gm.flags = (spec_tex_slot >= 3u) ? kRtGeomFlagBindlessTex : 0u;
            gm.tex_albedo = spec_tex_slot;
            gm.tint[0] = gm.tint[1] = gm.tint[2] = 1.0f;
            gm.tint[3] = 0.78f;
            gm.uv0_xform[0] = gm.uv0_xform[1] = gm.uv0_xform[2] = gm.uv0_xform[3] = 1.0f;
            spectator_blas_.geoms = {gm};
            spectator_blas_.vb_buf = spec_vb_.buffer;
            spectator_blas_.ib_buf = seq_ib_.buffer;
          }
        }
      }
    }
  } else if (spectator_blas_.handle) {
    destroy_blas(spectator_blas_, true);
  }

  // 3. Collect Airborne Showcase aircraft vertices (scenery space).
  const uint32_t show_n =
      (host.vfx_ready_ && host.showcase_buf_.ready() && host.showcase_count_ >= 3u)
          ? (std::min(host.showcase_count_, kMaxDynActorVerts) / 3u) * 3u
          : 0u;
  uint32_t show_tex_slot = 0u;
  if (show_n >= 3u && host.gpu_world_ && host.showcase_tex_.view) {
    constexpr uint32_t kShowcaseTexRecord = 0xFFFFFFF1u;
    if (registered_showcase_view_ != host.showcase_tex_.view || !host.gpu_world_->has_texture(kShowcaseTexRecord)) {
      show_tex_slot = host.gpu_world_->add_texture(kShowcaseTexRecord, host.showcase_tex_.view);
      registered_showcase_view_ = host.showcase_tex_.view;
    } else {
      show_tex_slot = host.gpu_world_->texture_slot(kShowcaseTexRecord);
    }
  }

  const uint32_t total_v = drv_n + show_n;
  if (total_v == 0u) return;

  // Ensure persistent per-frame-slot host-visible vertex buffer capacity.
  if (!slot.dyn_vb.buffer || slot.dyn_vb_cap < total_v) {
    const uint32_t alloc_n = std::max(total_v, std::max(slot.dyn_vb_cap * 2u, 8192u));
    if (slot.dyn_vb_mapped) {
      vmaUnmapMemory(r_.allocator_, slot.dyn_vb.allocation);
      slot.dyn_vb_mapped = nullptr;
    }
    if (slot.dyn_vb.buffer) r_.destroy_buffer(slot.dyn_vb);
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!r_.create_buffer(VkDeviceSize(alloc_n) * sizeof(WorldVertex), usage,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, slot.dyn_vb)) {
      slot.dyn_vb_cap = 0;
      return;
    }
    if (vmaMapMemory(r_.allocator_, slot.dyn_vb.allocation, &slot.dyn_vb_mapped) != VK_SUCCESS) {
      r_.destroy_buffer(slot.dyn_vb);
      slot.dyn_vb_cap = 0;
      return;
    }
    slot.dyn_vb_cap = alloc_n;
  }

  auto* dst = static_cast<WorldVertex*>(slot.dyn_vb_mapped);
  const VkDeviceAddress base_bda = r_.buffer_device_address(slot.dyn_vb.buffer);
  uint32_t cursor = 0;

  auto build_dyn = [&](const WorldVertex* src_verts, uint32_t count, uint32_t mat_class, uint32_t tex_slot,
                       float rough, BlasHandle& out_blas) {
    if (count < 3u) return;
    const uint32_t v_off = cursor;
    std::memcpy(dst + cursor, src_verts, size_t(count) * sizeof(WorldVertex));
    const VkDeviceAddress v_bda = base_bda + uint64_t(cursor) * sizeof(WorldVertex);
    cursor += count;

    const uint32_t prim_count = count / 3u;
    VkAccelerationStructureGeometryKHR ag{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    ag.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    ag.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& tri = ag.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = v_bda;
    tri.vertexStride = sizeof(WorldVertex);
    tri.maxVertex = count - 1u;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = seq_ib_bda_;

    if (build_blas_from_geoms(cmd, {ag}, {prim_count}, {{prim_count, 0, 0, 0}}, false, out_blas, nullptr)) {
      RtGeomInfoGpu gm{};
      gm.vb_off = v_off;
      gm.ib_off = 0u;
      gm.material_class = mat_class;
      gm.flags = (tex_slot >= 3u) ? kRtGeomFlagBindlessTex : 0u;
      gm.tex_albedo = tex_slot;
      gm.tint[0] = gm.tint[1] = gm.tint[2] = 1.0f;
      gm.tint[3] = rough;
      gm.uv0_xform[0] = gm.uv0_xform[1] = gm.uv0_xform[2] = gm.uv0_xform[3] = 1.0f;
      out_blas.geoms = {gm};
      out_blas.vb_buf = slot.dyn_vb.buffer;
      out_blas.ib_buf = seq_ib_.buffer;
    }
  };

  if (drv_n >= 3u) {
    build_dyn(reinterpret_cast<const WorldVertex*>(host.driver_buf_.cpu.data()), drv_n, 4u, 0u, 0.65f,
              slot.driver_blas);
  }
  if (show_n >= 3u) {
    build_dyn(reinterpret_cast<const WorldVertex*>(host.showcase_buf_.cpu.data()), show_n, 100u, show_tex_slot, 0.32f,
              slot.showcase_blas);
  }
}

bool RtScene::ensure_slot_buffers(FrameSlot& s, uint32_t inst_n, uint32_t geom_n, uint32_t light_n) {
  auto ensure_mapped = [&](Renderer::Buffer& buf, void*& mapped, uint32_t& cap, uint32_t need, size_t elem_bytes,
                           VkBufferUsageFlags usage) -> bool {
    const uint32_t want = std::max(need, 16u);
    if (buf.buffer && cap >= want) return true;
    const uint32_t alloc_n = std::max(want, cap * 2u);
    if (mapped) {
      vmaUnmapMemory(r_.allocator_, buf.allocation);
      mapped = nullptr;
    }
    if (buf.buffer) r_.destroy_buffer(buf);
    if (!r_.create_buffer(VkDeviceSize(alloc_n) * elem_bytes, usage,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf)) {
      cap = 0;
      return false;
    }
    if (vmaMapMemory(r_.allocator_, buf.allocation, &mapped) != VK_SUCCESS) {
      r_.destroy_buffer(buf);
      cap = 0;
      return false;
    }
    cap = alloc_n;
    return true;
  };

  const VkBufferUsageFlags vk_inst_usage =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (!ensure_mapped(s.inst_vk_buf, s.inst_vk_mapped, s.inst_vk_cap, inst_n,
                     sizeof(VkAccelerationStructureInstanceKHR), vk_inst_usage))
    return false;
  if (!ensure_mapped(s.inst_info_buf, s.inst_info_mapped, s.inst_info_cap, inst_n, sizeof(RtInstanceInfoGpu),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
    return false;
  if (!ensure_mapped(s.geom_info_buf, s.geom_info_mapped, s.geom_info_cap, geom_n, sizeof(RtGeomInfoGpu),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
    return false;
  if (!ensure_mapped(s.light_buf, s.light_mapped, s.light_cap, light_n, sizeof(RtLightGpu),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
    return false;
  return true;
}

void RtScene::record_update(VkCommandBuffer cmd, const Showroom& host) {
  if (!ready_) return;
  active_slot_ = r_.frame_slot();
  ++frame_counter_;
  scratch_alloc_.begin_frame();

  // 1. Process deferred BLAS compactions from completed frames.
  process_compactions(cmd);

  // 2. Ensure modular car BLASes FIRST so the hero car (body, glass, all wheels/tyres) is always built on frame 0.
  if (host.draw_car_) {
    ensure_car_blas(cmd, host, car_blases_[&host]);
  }
  for (const Showroom::Instance& inst : host.instances_) {
    if (inst.car) ensure_car_blas(cmd, *inst.car, car_blases_[inst.car]);
  }

  // 3. Update content-keyed static world BLASes, tree cross-card BLAS, and dynamic actor BLASes
  //    (seated 3D driver, grounded Spectators.zip crowds, and Airborne Showcase aircraft).
  update_world_blases(cmd, host);
  update_tree_blas(cmd, host);
  update_dynamic_actor_blases(cmd, host, slots_[active_slot_]);

  // Barrier: all BLAS builds & compactions must complete before TLAS build reads them.
  as_barrier(cmd,
             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR,
             VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
             VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);

  // 4. Assemble per-frame TLAS instances + RtInstanceInfo + RtGeomInfo + RtLightGpu
  //    and deduplicated bindless geometry storage buffer table (Set 2 binding 0).
  std::vector<VkAccelerationStructureInstanceKHR> vk_insts;
  std::vector<RtInstanceInfoGpu> inst_infos;
  std::vector<RtGeomInfoGpu> geom_infos;
  vk_insts.reserve(512);
  inst_infos.reserve(512);
  geom_infos.reserve(2048);

  auto& slot_bufs = slots_[active_slot_].geom_buffers;
  slot_bufs.clear();
  slot_bufs.reserve(64);
  auto register_geom_buffer = [&](VkBuffer buf) -> uint32_t {
    if (!buf) return UINT32_MAX;
    for (uint32_t i = 0; i < uint32_t(slot_bufs.size()); ++i) {
      if (slot_bufs[i] == buf) return i;
    }
    if (slot_bufs.size() >= kMaxRtGeomBuffers) return UINT32_MAX;
    const uint32_t idx = uint32_t(slot_bufs.size());
    slot_bufs.push_back(buf);
    return idx;
  };

  auto push_instance = [&](const BlasHandle& bh, const float model[16], uint8_t mask, rt::InstanceKind kind,
                           uint32_t car_inst_idx, bool force_opaque) {
    if (!bh.handle || !bh.address || bh.geoms.empty() || !bh.vb_buf || !bh.ib_buf) return;
    const uint32_t info_idx = uint32_t(inst_infos.size());
    if (info_idx >= rt::kInstanceInfoIndexMask) return;
    const uint32_t vb_slot = register_geom_buffer(bh.vb_buf);
    const uint32_t ib_slot = register_geom_buffer(bh.ib_buf);
    if (vb_slot == UINT32_MAX || ib_slot == UINT32_MAX) return;

    const uint32_t g_off = uint32_t(geom_infos.size());
    geom_infos.insert(geom_infos.end(), bh.geoms.begin(), bh.geoms.end());
    for (size_t gi = g_off; gi < geom_infos.size(); ++gi) {
      geom_infos[gi].vb_slot = vb_slot;
      geom_infos[gi].ib_slot = ib_slot;
    }

    RtInstanceInfoGpu ii{};
    ii.geom_offset = g_off;
    ii.geom_count = uint32_t(bh.geoms.size());
    ii.kind = uint32_t(kind);
    ii.car_inst_idx = car_inst_idx;
    fill_inst_rows(model, ii);
    inst_infos.push_back(ii);

    VkAccelerationStructureInstanceKHR vi{};
    vi.transform = to_vk_transform(model);
    vi.instanceCustomIndex = rt::pack_instance_custom_index(info_idx, kind);
    vi.mask = mask;
    vi.instanceShaderBindingTableRecordOffset = 0;
    vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
               (force_opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : 0u);
    vi.accelerationStructureReference = bh.address;
    vk_insts.push_back(vi);
  };

  // 4a. Static Colorado world cells.
  const float* wt = host.world_transform_;
  const double wox = host.world_origin_[0];
  const double woz = host.world_origin_[1];
  const WorldGpu* gpu_w = host.gpu_world_.get();
  for (uint64_t bkey : active_world_keys_) {
    auto it = world_blases_.find(bkey);
    if (it == world_blases_.end() || !it->second.blas.handle) continue;
    WorldCellEntry& cell = it->second;
    // Refresh bindless texture slots for any geometry whose textures streamed in after BLAS build.
    if (cell.blas.any_tex_pending && gpu_w && cell.blas.tex_refs.size() == cell.blas.geoms.size()) {
      bool still_pending = false;
      for (size_t gi = 0; gi < cell.blas.geoms.size(); ++gi) {
        auto& tr = cell.blas.tex_refs[gi];
        if (!tr.pending) continue;
        auto& gm = cell.blas.geoms[gi];
        bool g_pend = false;
        gm.tex_albedo = resolve_world_tex_slot(gpu_w, tr.albedo, g_pend);
        if (gm.tex_albedo != 0u) gm.flags |= kRtGeomFlagBindlessTex;
        gm.tex_aux = resolve_world_tex_slot(gpu_w, tr.aux, g_pend);
        gm.extra_tex[0] = resolve_world_tex_slot(gpu_w, tr.splat, g_pend);
        gm.extra_tex[1] = resolve_world_tex_slot(gpu_w, tr.layer_c, g_pend);
        gm.extra_tex[3] = resolve_world_tex_slot(gpu_w, tr.ao, g_pend);
        tr.pending = g_pend;
        if (g_pend) still_pending = true;
      }
      cell.blas.any_tex_pending = still_pending;
    }
    const float sx = float(cell.origin_x - wox);
    // Lift road decal cells +1.2 cm above the asphalt in Y so lane lines never z-fight in BVH.
    const float sy = (cell.opacity_class == 3u) ? 0.012f : 0.0f;
    const float sz = float(cell.origin_z - woz);
    float shift_m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, sx, sy, sz, 1};
    float cell_world[16];
    mat4_mul(wt, shift_m, cell_world);
    // Road decals use kMaskEmissive (included in primary + reflection rays, excluded from shadow casters).
    const uint8_t mask = (cell.opacity_class == 2u)
                             ? rt::kMaskWater
                             : (cell.opacity_class == 3u
                                    ? rt::kMaskEmissive
                                    : (cell.opacity_class == 1u ? rt::kMaskFoliageAlpha : rt::kMaskWorldOpaque));
    const bool force_opaque = (cell.opacity_class == 0u || cell.opacity_class == 2u);
    push_instance(cell.blas, cell_world, mask,
                  cell.opacity_class == 1u ? rt::InstanceKind::kFoliage : rt::InstanceKind::kWorld, 0u, force_opaque);
  }

  // 4b. Colorado tree cross-quad BLAS.
  if (tree_blas_.handle) {
    const float sx = float(tree_origin_x_ - wox);
    const float sz = float(tree_origin_z_ - woz);
    float shift_m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, sx, 0, sz, 1};
    float tree_world[16];
    mat4_mul(wt, shift_m, tree_world);
    push_instance(tree_blas_, tree_world, rt::kMaskFoliageAlpha, rt::InstanceKind::kFoliage, 0u, false);
  }

  // 4b-2. Dynamic actors: Spectators.zip festival crowds & Airborne Showcase aircraft.
  const FrameSlot& cur_slot = slots_[active_slot_];
  if (spectator_blas_.handle) {
    push_instance(spectator_blas_, wt, rt::kMaskWorldOpaque, rt::InstanceKind::kWorld, 0u, true);
  }
  if (cur_slot.showcase_blas.handle) {
    push_instance(cur_slot.showcase_blas, wt, rt::kMaskWorldOpaque, rt::InstanceKind::kWorld, 0u, true);
  }

  // 4c. Player car (body + glass + 4 spinning wheels + 4 calipers + live lamp emission + seated driver).
  auto emit_car = [&](const Showroom& car, const float* parent_model, const drive::LampShaderVectors& active_lamps,
                      uint8_t body_mask, rt::InstanceKind kind, uint32_t car_id, bool emit_detail) {
    auto it = car_blases_.find(&car);
    if (it == car_blases_.end()) return;
    const CarBlasSet& cs = it->second;
    const auto& src_draws = cs.use_far ? car.far_draws_ : car.draws_;
    auto combine = [&](const float local[16], float out[16]) {
      if (parent_model) mat4_mul(parent_model, local, out);
      else std::memcpy(out, local, 16 * sizeof(float));
    };
    float m[16];
    const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float* body_local =
        (cs.body_draw >= 0 && size_t(cs.body_draw) < src_draws.size()) ? src_draws[size_t(cs.body_draw)].push.model
                                                                       : kIdent;
    const float* glass_local =
        (cs.glass_draw >= 0 && size_t(cs.glass_draw) < src_draws.size()) ? src_draws[size_t(cs.glass_draw)].push.model
                                                                         : body_local;
    combine(body_local, m);
    const size_t body_g_start = geom_infos.size();
    push_instance(cs.body, m, body_mask, kind, car_id, true);
    // Gate car lamp emission dynamically on live braking/running-lamp state (matching raster!).
    if (cs.body_lamp_channels.size() == cs.body.geoms.size() &&
        body_g_start + cs.body.geoms.size() <= geom_infos.size()) {
      for (size_t gi = 0; gi < cs.body.geoms.size(); ++gi) {
        const auto ch = drive::LampChannel(cs.body_lamp_channels[gi]);
        if (ch == drive::LampChannel::kNone) continue;
        RtGeomInfoGpu& gm = geom_infos[body_g_start + gi];
        const auto rgb = drive::evaluate_lamp_emission(ch, active_lamps, car.lamp_colors_);
        if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
          gm.flags |= kRtGeomFlagEmissive;
          if (gm.tex_aux == 1u) {
            // Saturated crimson ruby HDR red so tonemapping never shifts taillights to yellow-orange.
            gm.tint[0] = std::min(rgb[0] * 1.15f, 2.3f);
            gm.tint[1] = std::min(rgb[1] * 0.12f, 0.022f);
            gm.tint[2] = std::min(rgb[2] * 0.10f, 0.010f);
          } else {
            gm.tint[0] = std::min(rgb[0] * 1.65f, 5.2f);
            gm.tint[1] = std::min(rgb[1] * 1.65f, 5.0f);
            gm.tint[2] = std::min(rgb[2] * 1.65f, 4.6f);
          }
        } else {
          gm.flags &= ~kRtGeomFlagEmissive;
        }
      }
    }
    if (emit_detail) {
      if (cs.glass.handle) {
        combine(glass_local, m);
        push_instance(cs.glass, m, rt::kMaskGlass, rt::InstanceKind::kGlass, car_id, false);
      }
      for (const auto& wp : cs.wheel_parts) {
        if (wp.blas.handle && wp.draw_idx >= 0 && size_t(wp.draw_idx) < src_draws.size()) {
          combine(src_draws[size_t(wp.draw_idx)].push.model, m);
          push_instance(wp.blas, m, body_mask, kind, car_id, true);
        }
      }
    }
  };

  const float kIdentMat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (host.draw_car_) {
    emit_car(host, nullptr, host.lamp_vectors_, rt::kMaskPlayerCar, rt::InstanceKind::kPlayerCar, 0u, true);
    if (!host.cockpit_camera_active_ && cur_slot.driver_blas.handle) {
      push_instance(cur_slot.driver_blas, kIdentMat, rt::kMaskPlayerCar, rt::InstanceKind::kPlayerCar, 0u, true);
    }
  }

  // 4d. Traffic / AI guest cars (+ seated drivers inside nearby cars), distance-culled past 140 m
  // and skipping glass/wheel sub-BLAS instances past 45 m to keep TLAS builds lean in races.
  uint32_t traffic_id = 1u;
  for (const Showroom::Instance& inst : host.instances_) {
    if (!inst.car) continue;
    const float dx = inst.model[12] - host.frame_.eye[0];
    const float dy = inst.model[13] - host.frame_.eye[1];
    const float dz = inst.model[14] - host.frame_.eye[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 <= 140.0f * 140.0f) {
      const bool detail = d2 <= 45.0f * 45.0f;
      const drive::LampShaderVectors& glamps = inst.has_lamps ? inst.lamps : inst.car->lamp_vectors_;
      emit_car(*inst.car, inst.model, glamps, rt::kMaskTrafficCar, rt::InstanceKind::kTrafficCar, traffic_id, detail);
      if (detail && cur_slot.driver_blas.handle && inst.fade_alpha >= 0.5f) {
        push_instance(cur_slot.driver_blas, inst.model, rt::kMaskTrafficCar, rt::InstanceKind::kTrafficCar, traffic_id,
                      true);
      }
    }
    ++traffic_id;
  }

  // 4e. Dynamic Light List (Light 0 = Sun, then headlights, brake/taillights, and world streetlamps/Festival glows).
  std::vector<RtLightGpu> lights;
  lights.reserve(256);
  {
    // Light 0: Analytical Sun directional disc.
    RtLightGpu sun{};
    const float* sd = host.frame_.key_light;
    const float slen = std::max(std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]), 1e-4f);
    sun.pos_type[0] = sd[0] / slen;
    sun.pos_type[1] = sd[1] / slen;
    sun.pos_type[2] = sd[2] / slen;
    sun.pos_type[3] = 0.0f;  // directional sun
    sun.dir_range[0] = -sun.pos_type[0];
    sun.dir_range[1] = -sun.pos_type[1];
    sun.dir_range[2] = -sun.pos_type[2];
    sun.dir_range[3] = 10000.0f;
    const bool has_tod = host.frame_.sun_color[3] >= 0.0f;
    sun.color_power[0] = has_tod ? host.frame_.sun_color[0] : 0.9695f;
    sun.color_power[1] = has_tod ? host.frame_.sun_color[1] : 0.8358f;
    sun.color_power[2] = has_tod ? host.frame_.sun_color[2] : 0.5840f;
    sun.color_power[3] = 1.0f;
    sun.extra[0] = 0.00465f;  // tan(0.266 deg) solar angular radius
    sun.extra[1] = 0.00465f;
    lights.push_back(sun);
  }
  const float night_factor = (host.frame_.sun_color[3] >= 0.0f) ? host.frame_.sky_color[3] : 0.0f;
  // Helper: emits headlights + taillights for a car with live body matrix `bm`.
  auto emit_car_lights = [&](const float bm[16], float owner_id, float brake_amt, float hl_scale, float hl_range) {
    auto xform_pt = [&](float x, float y, float z, float out[3]) {
      out[0] = bm[0] * x + bm[4] * y + bm[8] * z + bm[12];
      out[1] = bm[1] * x + bm[5] * y + bm[9] * z + bm[13];
      out[2] = bm[2] * x + bm[6] * y + bm[10] * z + bm[14];
    };
    auto xform_dir = [&](float x, float y, float z, float out[3]) {
      out[0] = bm[0] * x + bm[4] * y + bm[8] * z;
      out[1] = bm[1] * x + bm[5] * y + bm[9] * z;
      out[2] = bm[2] * x + bm[6] * y + bm[10] * z;
      const float l = std::max(std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]), 1e-4f);
      out[0] /= l; out[1] /= l; out[2] /= l;
    };
    if (night_factor > 0.05f && lights.size() + 2u < 192u) {
      for (int side = -1; side <= 1; side += 2) {
        RtLightGpu hl{};
        xform_pt(0.68f * float(side), 0.65f, -2.05f, hl.pos_type);
        hl.pos_type[3] = 1.0f;  // spotlight
        xform_dir(0.04f * float(side), -0.07f, -1.0f, hl.dir_range);
        hl.dir_range[3] = hl_range;
        hl.color_power[0] = 5.5f * hl_scale * night_factor;
        hl.color_power[1] = 5.2f * hl_scale * night_factor;
        hl.color_power[2] = 4.6f * hl_scale * night_factor;
        hl.color_power[3] = 0.72f;  // outer cone cos (~44 deg)
        hl.extra[0] = 0.92f;        // inner cone cos (~23 deg)
        hl.extra[1] = 0.10f;
        hl.extra[2] = owner_id;
        lights.push_back(hl);
      }
    }
    const float brake_glow = std::max(brake_amt, night_factor * 0.45f);
    if (brake_glow > 0.05f && lights.size() + 2u < 192u) {
      for (int side = -1; side <= 1; side += 2) {
        RtLightGpu tl{};
        xform_pt(0.65f * float(side), 0.76f, 2.08f, tl.pos_type);
        tl.pos_type[3] = 2.0f;  // point/disc taillight
        xform_dir(0.0f, 0.0f, 1.0f, tl.dir_range);
        tl.dir_range[3] = 14.0f;
        tl.color_power[0] = 3.2f * brake_glow;
        tl.color_power[1] = 0.14f * brake_glow;
        tl.color_power[2] = 0.06f * brake_glow;
        tl.color_power[3] = 1.0f;
        tl.extra[1] = 0.08f;
        tl.extra[2] = owner_id;
        lights.push_back(tl);
      }
    }
  };
  // Player car headlight beams + brake lights transformed by live body_local matrix.
  const float player_brake = host.lamp_vectors_.dynamic_lights_amount[0];
  if (host.draw_car_ && (night_factor > 0.05f || player_brake > 0.1f)) {
    const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float* bm = kIdent;
    if (auto it = car_blases_.find(&host); it != car_blases_.end() && it->second.body_draw >= 0 &&
                                           size_t(it->second.body_draw) < host.draws_.size()) {
      bm = host.draws_[size_t(it->second.body_draw)].push.model;
    }
    emit_car_lights(bm, 0.0f, player_brake, 1.0f, 65.0f);
  }
  // Traffic & AI guest car headlights + taillights (within 120 m of eye).
  if (night_factor > 0.05f) {
    uint32_t gid = 1u;
    for (const Showroom::Instance& inst : host.instances_) {
      if (!inst.car) continue;
      const float dx = inst.model[12] - host.frame_.eye[0];
      const float dy = inst.model[13] - host.frame_.eye[1];
      const float dz = inst.model[14] - host.frame_.eye[2];
      if (dx * dx + dy * dy + dz * dz <= 120.0f * 120.0f) {
        const float gb = inst.has_lamps ? inst.lamps.dynamic_lights_amount[0] : 0.0f;
        emit_car_lights(inst.model, float(gid), gb, 0.85f, 48.0f);
      }
      ++gid;
    }
  }
  // Colorado world streetlamps, outposts & Horizon Festival glows (within 180 m).
  if (night_factor > 0.05f && !host.world_glows_.empty()) {
    for (const world::WorldGlow& wg : host.world_glows_) {
      if (lights.size() >= 192u) break;
      const float gx = wg.position[0], gy = wg.position[1], gz = wg.position[2];
      const float rx = wt[0] * gx + wt[4] * gy + wt[8] * gz + wt[12];
      const float ry = wt[1] * gx + wt[5] * gy + wt[9] * gz + wt[13];
      const float rz = wt[2] * gx + wt[6] * gy + wt[10] * gz + wt[14];
      const float dx = rx - host.frame_.eye[0], dy = ry - host.frame_.eye[1], dz = rz - host.frame_.eye[2];
      if (dx * dx + dy * dy + dz * dz > 180.0f * 180.0f) continue;
      RtLightGpu gl{};
      gl.pos_type[0] = rx; gl.pos_type[1] = ry; gl.pos_type[2] = rz; gl.pos_type[3] = 3.0f;
      gl.dir_range[0] = 0.0f; gl.dir_range[1] = -1.0f; gl.dir_range[2] = 0.0f;
      gl.dir_range[3] = std::clamp(wg.radius * 7.5f, 12.0f, 45.0f);
      gl.color_power[0] = wg.color[0] * 4.0f * night_factor;
      gl.color_power[1] = wg.color[1] * 4.0f * night_factor;
      gl.color_power[2] = wg.color[2] * 4.0f * night_factor;
      gl.color_power[3] = 1.0f;
      gl.extra[1] = std::max(wg.radius * 0.25f, 0.15f);
      lights.push_back(gl);
    }
  }

  // 5. Upload slot buffers and rebuild this frame slot's TLAS.
  FrameSlot& slot = slots_[active_slot_];
  const uint32_t inst_n = uint32_t(vk_insts.size());
  const uint32_t geom_n = uint32_t(geom_infos.size());
  const uint32_t light_n = uint32_t(lights.size());
  if (!ensure_slot_buffers(slot, inst_n, geom_n, light_n)) return;

  if (inst_n > 0) {
    std::memcpy(slot.inst_vk_mapped, vk_insts.data(), inst_n * sizeof(VkAccelerationStructureInstanceKHR));
    std::memcpy(slot.inst_info_mapped, inst_infos.data(), inst_n * sizeof(RtInstanceInfoGpu));
  }
  if (geom_n > 0) {
    std::memcpy(slot.geom_info_mapped, geom_infos.data(), geom_n * sizeof(RtGeomInfoGpu));
  }
  if (light_n > 0) {
    std::memcpy(slot.light_mapped, lights.data(), light_n * sizeof(RtLightGpu));
  }

  VkAccelerationStructureGeometryKHR tlas_geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlas_geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  tlas_geom.geometry.instances.arrayOfPointers = VK_FALSE;
  tlas_geom.geometry.instances.data.deviceAddress = r_.buffer_device_address(slot.inst_vk_buf.buffer);

  VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlas_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  tlas_build.geometryCount = 1;
  tlas_build.pGeometries = &tlas_geom;

  VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(r_.device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_build,
                                          &inst_n, &tlas_sizes);

  if (!slot.tlas || slot.tlas_buf.size < tlas_sizes.accelerationStructureSize) {
    if (slot.tlas) vkDestroyAccelerationStructureKHR(r_.device_, slot.tlas, nullptr);
    slot.tlas = VK_NULL_HANDLE;
    r_.destroy_buffer(slot.tlas_buf);
    const VkDeviceSize alloc_sz = std::max(tlas_sizes.accelerationStructureSize, VkDeviceSize(65536));
    if (!r_.create_buffer(alloc_sz,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, slot.tlas_buf)) {
      return;
    }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = slot.tlas_buf.buffer;
    ci.size = alloc_sz;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(r_.device_, &ci, nullptr, &slot.tlas) != VK_SUCCESS) {
      r_.destroy_buffer(slot.tlas_buf);
      return;
    }
  }
  const VkDeviceSize scratch_need = std::max(tlas_sizes.buildScratchSize, VkDeviceSize(65536));
  if (!slot.tlas_scratch.buffer || slot.tlas_scratch.size < scratch_need) {
    r_.destroy_buffer(slot.tlas_scratch);
    if (!r_.create_buffer(scratch_need,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, slot.tlas_scratch)) {
      return;
    }
  }

  tlas_build.dstAccelerationStructure = slot.tlas;
  tlas_build.scratchData.deviceAddress = r_.buffer_device_address(slot.tlas_scratch.buffer);
  VkAccelerationStructureBuildRangeInfoKHR tlas_range{inst_n, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* p_tlas_range = &tlas_range;
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlas_build, &p_tlas_range);

  // Barrier: TLAS + SSBO writes visible to RT/PT compute passes.
  as_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
             VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

  // Update telemetry stats.
  stats_ = {};
  stats_.world_blas_count = uint32_t(world_blases_.size());
  stats_.car_blas_count = uint32_t(car_blases_.size());
  stats_.tlas_instances = inst_n;
  stats_.geom_count = geom_n;
  stats_.light_count = light_n;
  for (const auto& [k, e] : world_blases_) {
    stats_.blas_bytes += e.blas.bytes;
    if (e.blas.compacted) {
      ++stats_.world_blas_compacted;
      if (e.blas.uncompacted_bytes > e.blas.bytes) stats_.blas_saved_bytes += (e.blas.uncompacted_bytes - e.blas.bytes);
    }
  }
  for (const auto& [c, cs] : car_blases_) {
    stats_.blas_bytes += cs.body.bytes + cs.glass.bytes;
    for (const auto& wp : cs.wheel_parts) stats_.blas_bytes += wp.blas.bytes;
  }
}

}  // namespace carview

#else  // !SHIFT_ENABLE_RAY_TRACING

namespace carview {
RtScene::RtScene(Renderer& r) : r_(r) {}
RtScene::~RtScene() {}
bool RtScene::init(RetireFn, std::string& error) {
  error = "ray tracing disabled at compile time";
  return false;
}
void RtScene::shutdown() {}
void RtScene::invalidate_car(const Showroom*) {}
void RtScene::invalidate_world() {}
void RtScene::record_update(VkCommandBuffer, const Showroom&) {}
}  // namespace carview

#endif
