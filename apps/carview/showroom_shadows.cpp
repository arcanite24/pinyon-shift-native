#include "showroom_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace carview {
namespace {

constexpr uint32_t kMaxShadowTrees = 512;
// Cross-quad (X-card: 2 orthogonal vertical cards) + horizontal canopy crown cap = 3 quads (18 verts) per tree
// so noon sun casts full dappled foliage shadows instead of razor-thin vertical slivers.
constexpr uint32_t kVertsPerShadowTree = 18u;
constexpr uint32_t kMaxShadowTreeVerts = kMaxShadowTrees * kVertsPerShadowTree;

struct ShadowTreeBatch {
  uint32_t texture = kNoWorldTexture;
  uint32_t first_vertex = 0;
  uint32_t vertex_count = 0;
};

struct ShadowTreeCache {
  Renderer::Buffer vb;
  void* mapped = nullptr;
  std::vector<WorldVertex> cpu;
  std::vector<ShadowTreeBatch> batches;
  uint64_t world_gen = 0;
  float eye_sc[3] = {1e9f, 1e9f, 1e9f};
  float sun_sc[3] = {0.0f, 1.0f, 0.0f};
  uint64_t ver = 0;
  uint64_t slot_ver[Renderer::kFramesInFlight] = {};
};

std::mutex g_shadow_tree_mutex;
std::unordered_map<const Showroom*, std::unique_ptr<ShadowTreeCache>> g_shadow_tree_caches;

}  // namespace

bool Showroom::shadows_on() const {
  return shadow_set_ && shadow_world_ && r_.shadow_size_ > 1 && world_textured_ && !world_draws_.empty();
}

void Showroom::place_shadow() {
  const ShadowSettings& s = r_.options_.shadow;
  const auto pcss_q = r_.graphics_settings().shadow_pcss;
  const bool three_cascades = (r_.shadow_cascades_ >= 3u) && (shadow_set_mid_ != VK_NULL_HANDLE) &&
                              (shadow_set_far_ != VK_NULL_HANDLE);
  // Wire parsed TrackSettings.xml ShadowEnds (ShadowNear / InGameShadowEnd) into stabilized CSM splits.
  const float parsed_near =
      tod_active_ ? std::clamp(tod_state_.shadow_near, 16.0f, 40.0f) : (three_cascades ? 20.0f : std::min(s.radius, 30.0f));
  const float parsed_far =
      tod_active_ ? std::clamp(tod_state_.shadow_far, 120.0f, 360.0f)
                  : (three_cascades ? 320.0f : std::max(s.radius * 3.0f, 185.0f));
  const float near_radius = three_cascades ? parsed_near : std::min(s.radius, parsed_near);
  const float far_radius = three_cascades
                               ? (pcss_q == modern::ShadowPcss::kUltra ? std::max(parsed_far, 260.0f) : parsed_far)
                               : std::max(s.radius * 3.0f, parsed_far);
  const float mid_radius = std::clamp(std::sqrt(near_radius * far_radius), near_radius * 2.2f, far_radius * 0.55f);
  constexpr float kDepth = 520.0f;
  auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  auto normalise = [&](float* v) {
    const float l = std::sqrt(dot3(v, v));
    if (l < 1e-6f) return false;
    for (int k = 0; k < 3; ++k) v[k] /= l;
    return true;
  };
  auto cross = [](const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
  };
  const float* from = shadow_direction_set_ ? shadow_direction_ : world_sun_;
  float sun[3] = {from[0], from[1], from[2]};
  float up[3] = {world_up_[0], world_up_[1], world_up_[2]};
  if (!normalise(sun)) sun[0] = 0, sun[1] = 1, sun[2] = 0;
  if (!normalise(up)) up[0] = 0, up[1] = 1, up[2] = 0;
  const float* vp = frame_.view_projection;
  float ahead[3] = {vp[3], vp[7], vp[11]};
  const float along_up = dot3(ahead, up);
  for (int k = 0; k < 3; ++k) ahead[k] -= along_up * up[k];
  if (!normalise(ahead)) ahead[0] = 0, ahead[1] = 0, ahead[2] = 1;
  const float forward[3] = {-sun[0], -sun[1], -sun[2]};
  float right[3], across[3];
  cross(forward, up, right);
  if (!normalise(right)) {
    const float x[3] = {1, 0, 0};
    cross(forward, x, right);
    normalise(right);
  }
  cross(right, forward, across);
  // Anchor texel grid to absolute world origin (folding scenery origin shift) so camera
  // movement AND cell regathers produce zero shadow-edge shimmer!
  const float world_zero[3] = {
      world_transform_[12] - world_transform_[0] * float(world_origin_[0]) - world_transform_[8] * float(world_origin_[1]),
      world_transform_[13] - world_transform_[1] * float(world_origin_[0]) - world_transform_[9] * float(world_origin_[1]),
      world_transform_[14] - world_transform_[2] * float(world_origin_[0]) - world_transform_[10] * float(world_origin_[1]),
  };
  const float ox = dot3(world_zero, right), oy = dot3(world_zero, across);

  auto build_cascade = [&](float radius, float ahead_bias, float m[16], float& texel_out, float centre_out[3]) {
    for (int k = 0; k < 3; ++k) centre_out[k] = frame_.eye[k] + ahead[k] * radius * ahead_bias;
    const float texel = 2.0f * radius / float(r_.shadow_size_);
    texel_out = texel;
    float cx = dot3(centre_out, right), cy = dot3(centre_out, across);
    cx = ox + std::round((cx - ox) / texel) * texel;
    cy = oy + std::round((cy - oy) / texel) * texel;
    const float cz = dot3(centre_out, sun);
    std::memset(m, 0, 16 * sizeof(float));
    for (int k = 0; k < 3; ++k) {
      m[k * 4 + 0] = right[k] / radius;
      m[k * 4 + 1] = across[k] / radius;
      m[k * 4 + 2] = -sun[k] / (2.0f * kDepth);
    }
    m[12] = -cx / radius;
    m[13] = -cy / radius;
    m[14] = 0.5f + cz / (2.0f * kDepth);
    m[15] = 1.0f;
  };

  float m_near[16], m_mid[16], m_far[16];
  float texel_near = 0.0f, texel_mid = 0.0f, texel_far = 0.0f;
  float centre_near[3], centre_mid[3], centre_far[3];
  build_cascade(near_radius, 0.42f, m_near, texel_near, centre_near);
  if (three_cascades) {
    build_cascade(mid_radius, 0.50f, m_mid, texel_mid, centre_mid);
  } else {
    std::memset(m_mid, 0, sizeof m_mid);
    centre_mid[0] = centre_mid[1] = centre_mid[2] = 0.0f;
  }
  build_cascade(far_radius, 0.52f, m_far, texel_far, centre_far);

  const bool atlas_reset = !shadow_atlas_valid_ || (shadow_atlas_world_gen_ != world_generation_);
#ifdef __ANDROID__
  // Amortize Mid and Far shadow cascades across alternating frames on Android while keeping
  // Near (~20-40 m around the player car) refreshed every frame (~40-50% less shadow GPU time).
  if (atlas_reset) {
    shadow_update_mid_ = true;
    shadow_update_far_ = true;
  } else if (three_cascades) {
    shadow_update_mid_ = ((world_frame_ & 1ull) == 0ull);
    shadow_update_far_ = ((world_frame_ & 1ull) == 1ull);
  } else {
    shadow_update_mid_ = false;
    shadow_update_far_ = ((world_frame_ & 1ull) == 0ull);
  }
#else
  // Desktop keeps Near and Mid refreshed every frame for crisp nearby shadows while
  // amortizing the 200m+ Far cascade every 2nd frame (~30% less shadow CPU/GPU work).
  shadow_update_mid_ = true;
  shadow_update_far_ = atlas_reset || ((world_frame_ & 1ull) == 0ull);
#endif
  if (three_cascades) {
    if (shadow_update_mid_) {
      std::memcpy(shadow_cached_mid_matrix_, m_mid, sizeof m_mid);
      shadow_cached_mid_bias_ = 1.45f * texel_mid;
    } else {
      std::memcpy(m_mid, shadow_cached_mid_matrix_, sizeof m_mid);
    }
  }
  if (shadow_update_far_) {
    std::memcpy(shadow_cached_far_matrix_, m_far, sizeof m_far);
    shadow_cached_far_bias_ = 1.55f * texel_far;
  } else {
    std::memcpy(m_far, shadow_cached_far_matrix_, sizeof m_far);
  }

  std::memcpy(frame_.shadow_matrix, m_near, sizeof m_near);
  std::memcpy(frame_.shadow_matrix_mid, m_mid, sizeof m_mid);
  std::memcpy(frame_.shadow_matrix_far, m_far, sizeof m_far);
  // Preserve full RGB TrackSettings ShadowColor in shadow_bias[3] (tagged with safe normal-float
  // exponent 0x3Fu so it never NaNs/denorms) and keep shadow[2] as shadow intensity [0.12..1.0].
  // Shaders keep sun_shadow()/drive_shadow() strictly in [0, 1] so tunnels/buildings leak 0 direct sun,
  // modulating only shadowed ambient/hemisphere light with RGB shadow_color.
  const float shadow_rgb[3] = {
      tod_active_ ? std::clamp(tod_state_.shadow_color[0], 0.0f, 1.0f) : std::clamp(s.strength, 0.0f, 1.0f),
      tod_active_ ? std::clamp(tod_state_.shadow_color[1], 0.0f, 1.0f) : std::clamp(s.strength, 0.0f, 1.0f),
      tod_active_ ? std::clamp(tod_state_.shadow_color[2], 0.0f, 1.0f) : std::clamp(s.strength, 0.0f, 1.0f),
  };
  const uint32_t sr = uint32_t(std::lround(shadow_rgb[0] * 255.0f)) & 0xFFu;
  const uint32_t sg = uint32_t(std::lround(shadow_rgb[1] * 255.0f)) & 0xFFu;
  const uint32_t sb = uint32_t(std::lround(shadow_rgb[2] * 255.0f)) & 0xFFu;
  const uint32_t packed_shadow_rgb = (0x3Fu << 24u) | (sb << 16u) | (sg << 8u) | sr;
  float shadow_rgb_bits = 0.0f;
  std::memcpy(&shadow_rgb_bits, &packed_shadow_rgb, sizeof(shadow_rgb_bits));
  const float shadow_intensity =
      tod_active_ ? std::clamp(0.5f * (tod_state_.shadow_intensity + 1.0f), 0.12f, 1.0f) : 1.0f;
  frame_.shadow[0] = 1.0f;
  frame_.shadow[1] = 1.0f / float(r_.shadow_size_);
  frame_.shadow[2] = shadow_intensity;
  frame_.shadow[3] = float(s.taps);
  frame_.shadow_bias[0] = 1.35f * texel_near;
  frame_.shadow_bias[1] = frame_.shadow_bias[2] = 0.0f;
  frame_.shadow_bias[3] = shadow_rgb_bits;
  frame_.shadow_mid_bias[0] = three_cascades ? shadow_cached_mid_bias_ : (1.45f * texel_mid);
  frame_.shadow_mid_bias[1] = three_cascades ? 1.0f : 0.0f;
  frame_.shadow_mid_bias[2] = (pcss_q == modern::ShadowPcss::kUltra) ? 2.0f
                            : (pcss_q == modern::ShadowPcss::kHigh)  ? 1.0f
                                                                     : 0.0f;
  frame_.shadow_mid_bias[3] = float(r_.shadow_cascades_);
  frame_.shadow_far_bias[0] = shadow_cached_far_bias_;
  frame_.shadow_far_bias[1] = shadow_set_far_ ? 1.0f : 0.0f;

  auto fill_shadow_frame = [&](ShowroomFrame& dst, const float m[16]) {
    dst = frame_;
    std::memcpy(dst.view_projection, m, 16 * sizeof(float));
    std::memcpy(dst.transparent_view_projection, m, 16 * sizeof(float));
    // Preserve camera eye and apply a crisp midpoint prop LOD split so LOD00 (kFlagLodNear) and
    // LOD02 (kFlagLodFar) are NEVER drawn simultaneously into the shadow map (no double overdraw / acne).
    if (frame_.world_fade[1] > 0.0f) {
      dst.world_fade[0] = frame_.world_fade[0] + 0.5f * frame_.world_fade[1];
      dst.world_fade[1] = 0.01f;
    }
  };
  fill_shadow_frame(shadow_frame_, m_near);
  if (three_cascades) fill_shadow_frame(shadow_frame_mid_, m_mid);
  fill_shadow_frame(shadow_frame_far_, m_far);
}

void Showroom::record_shadow(VkCommandBuffer cmd) {
  const uint32_t size = r_.shadow_size_;
  const uint32_t cascades = std::max(1u, r_.shadow_cascades_);
  const uint32_t atlas_w = size > 1 ? size * cascades : size;
  const bool partial_update =
      shadow_atlas_valid_ && (shadow_atlas_world_gen_ == world_generation_) &&
      ((cascades >= 3u && (!shadow_update_mid_ || !shadow_update_far_)) ||
       (cascades == 2u && !shadow_update_far_));
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  b.oldLayout = partial_update ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = r_.shadow_map_.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);

  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = r_.shadow_map_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = partial_update ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depthStencil = {1.0f, 0};
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {atlas_w, size}};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);

  shadow_drawn_ = shadow_cars_ = 0;
  const auto eye = eye_in_scenery(world_transform_);
  const bool ultra_pcss = r_.graphics_settings().shadow_pcss == modern::ShadowPcss::kUltra;
  const float lod_mid = (world_fade_[1] > 0.0f) ? (world_fade_[0] + 0.5f * world_fade_[1]) : 0.0f;

  // Build / refresh sun-facing alpha-tested shadow quads for GPU-instanced trees (`world_trees_`)
  // within Near/Mid shadow reach so Colorado forests cast dappled tree shadows onto roads & cars.
  ShadowTreeCache* tree_cache = nullptr;
  VkDeviceSize tree_vb_offset = 0;
#ifdef __ANDROID__
  const bool allow_shadow_cutouts = !low_memory_mode_;
#else
  const bool allow_shadow_cutouts = true;
#endif
  if (allow_shadow_cutouts && !world_trees_.empty() && shadow_cutout_) {
    const float* from = shadow_direction_set_ ? shadow_direction_ : world_sun_;
    float sun_sc[3] = {
        world_transform_[0] * from[0] + world_transform_[1] * from[1] + world_transform_[2] * from[2],
        world_transform_[4] * from[0] + world_transform_[5] * from[1] + world_transform_[6] * from[2],
        world_transform_[8] * from[0] + world_transform_[9] * from[1] + world_transform_[10] * from[2],
    };
    const float sl = std::sqrt(sun_sc[0] * sun_sc[0] + sun_sc[1] * sun_sc[1] + sun_sc[2] * sun_sc[2]);
    if (sl > 1e-5f) sun_sc[0] /= sl, sun_sc[1] /= sl, sun_sc[2] /= sl;
    else sun_sc[0] = 0.0f, sun_sc[1] = 1.0f, sun_sc[2] = 0.0f;

    std::lock_guard<std::mutex> lock(g_shadow_tree_mutex);
    auto& ptr = g_shadow_tree_caches[this];
    if (!ptr) {
      auto c = std::make_unique<ShadowTreeCache>();
      const VkDeviceSize region = VkDeviceSize(kMaxShadowTreeVerts) * sizeof(WorldVertex);
      if (r_.create_buffer(region * Renderer::kFramesInFlight, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, c->vb)) {
        if (vmaMapMemory(r_.allocator_, c->vb.allocation, &c->mapped) != VK_SUCCESS) {
          c->mapped = nullptr;
          r_.destroy_buffer(c->vb);
        }
      }
      if (c->mapped) {
        ptr = std::move(c);
        Renderer* rptr = &r_;
        const Showroom* key = this;
        retired_.push_back({std::numeric_limits<uint64_t>::max(), [rptr, key]() {
                              std::unique_ptr<ShadowTreeCache> doomed;
                              {
                                std::lock_guard<std::mutex> lk(g_shadow_tree_mutex);
                                auto jt = g_shadow_tree_caches.find(key);
                                if (jt != g_shadow_tree_caches.end()) {
                                  doomed = std::move(jt->second);
                                  g_shadow_tree_caches.erase(jt);
                                }
                              }
                              if (doomed && doomed->vb.allocation) {
                                if (doomed->mapped) vmaUnmapMemory(rptr->allocator_, doomed->vb.allocation);
                                rptr->destroy_buffer(doomed->vb);
                              }
                            }});
      }
    }
    if (ptr && ptr->mapped) {
      tree_cache = ptr.get();
      const float moved2 = (eye[0] - tree_cache->eye_sc[0]) * (eye[0] - tree_cache->eye_sc[0]) +
                           (eye[1] - tree_cache->eye_sc[1]) * (eye[1] - tree_cache->eye_sc[1]) +
                           (eye[2] - tree_cache->eye_sc[2]) * (eye[2] - tree_cache->eye_sc[2]);
      const float sun_dot = sun_sc[0] * tree_cache->sun_sc[0] + sun_sc[1] * tree_cache->sun_sc[1] +
                            sun_sc[2] * tree_cache->sun_sc[2];
      if (tree_cache->world_gen != world_generation_ || moved2 > 9.0f || sun_dot < 0.998f || tree_cache->ver == 0) {
        tree_cache->world_gen = world_generation_;
        for (int k = 0; k < 3; ++k) tree_cache->eye_sc[k] = eye[size_t(k)], tree_cache->sun_sc[k] = sun_sc[k];
        const float max_tree_r = low_memory_mode_ ? 55.0f : 95.0f;
        const float max_tree_r2 = max_tree_r * max_tree_r;
        const size_t cap_trees = low_memory_mode_ ? 256u : kMaxShadowTrees;
        struct Cand {
          float d2;
          uint32_t tex;
          size_t idx;
        };
        std::vector<Cand> cands;
        cands.reserve(512);
        for (size_t i = 0; i < world_trees_.size(); ++i) {
          const auto& t = world_trees_[i];
          if (t.texture == kNoWorldTexture || t.half_width <= 0.01f || t.height <= 0.01f) continue;
          const float dx = t.base[0] - eye[0];
          const float dy = t.base[1] - eye[1];
          const float dz = t.base[2] - eye[2];
          const float d2 = dx * dx + dy * dy + dz * dz;
          if (d2 > max_tree_r2) continue;
          cands.push_back({d2, t.texture, i});
        }
        if (cands.size() > cap_trees) {
          std::nth_element(cands.begin(), cands.begin() + ptrdiff_t(cap_trees), cands.end(),
                           [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
          cands.resize(cap_trees);
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.tex < b.tex; });
        tree_cache->cpu.clear();
        tree_cache->cpu.reserve(cands.size() * kVertsPerShadowTree);
        tree_cache->batches.clear();
        static constexpr int kQuad[6] = {0, 1, 2, 0, 2, 3};
        for (const auto& c : cands) {
          const auto& t = world_trees_[c.idx];
          float u[3] = {t.up[0], t.up[1], t.up[2]};
          const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
          if (ul > 1e-4f) u[0] /= ul, u[1] /= ul, u[2] /= ul;
          else u[0] = 0.0f, u[1] = 1.0f, u[2] = 0.0f;
          // Build orthonormal horizontal basis (r, f) perpendicular to trunk axis u for cross-quad (X-card) geometry.
          float rx = u[1] * sun_sc[2] - u[2] * sun_sc[1];
          float ry = u[2] * sun_sc[0] - u[0] * sun_sc[2];
          float rz = u[0] * sun_sc[1] - u[1] * sun_sc[0];
          const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
          if (rl > 1e-4f) {
            rx /= rl, ry /= rl, rz /= rl;
          } else {
            // Near-zenith noon sun: pick stable horizontal reference perpendicular to u.
            const float ref_x = std::fabs(u[0]) < 0.9f ? 1.0f : 0.0f;
            const float ref_z = std::fabs(u[0]) < 0.9f ? 0.0f : 1.0f;
            rx = u[1] * ref_z;
            ry = u[2] * ref_x - u[0] * ref_z;
            rz = -u[1] * ref_x;
            const float fl = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (fl > 1e-4f) rx /= fl, ry /= fl, rz /= fl;
            else rx = 1.0f, ry = 0.0f, rz = 0.0f;
          }
          float fx = u[1] * rz - u[2] * ry;
          float fy = u[2] * rx - u[0] * rz;
          float fz = u[0] * ry - u[1] * rx;
          const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
          if (flen > 1e-4f) fx /= flen, fy /= flen, fz /= flen;
          else fx = 0.0f, fy = 0.0f, fz = 1.0f;

          const uint32_t first_v = uint32_t(tree_cache->cpu.size());
          auto emit_card = [&](float ax, float ay, float az) {
            for (int qi = 0; qi < 6; ++qi) {
              const int corner = kQuad[qi];
              const float ox = (corner == 1 || corner == 2) ? t.half_width : -t.half_width;
              const float oy = (corner >= 2) ? t.height : 0.0f;
              WorldVertex v{};
              v.position[0] = t.base[0] + ax * ox + u[0] * oy;
              v.position[1] = t.base[1] + ay * ox + u[1] * oy;
              v.position[2] = t.base[2] + az * ox + u[2] * oy;
              v.uv0[0] = (corner == 1 || corner == 2) ? t.uv[2] : t.uv[0];
              v.uv0[1] = (corner >= 2) ? t.uv[1] : t.uv[3];
              tree_cache->cpu.push_back(v);
            }
          };
          // Card 1 & Card 2: orthogonal vertical X-cards for full volumetric azimuth silhouettes.
          emit_card(rx, ry, rz);
          emit_card(fx, fy, fz);
          // Card 3: horizontal leafy crown cap in the upper canopy so high-noon sun casts full dappled shadows.
          const float crown_y = t.height * 0.58f;
          const float crown_r = t.half_width * 0.84f;
          const float crown_v_bot = t.uv[1] + (t.uv[3] - t.uv[1]) * 0.64f;
          for (int qi = 0; qi < 6; ++qi) {
            const int corner = kQuad[qi];
            const float ox = (corner == 1 || corner == 2) ? crown_r : -crown_r;
            const float oz = (corner >= 2) ? crown_r : -crown_r;
            WorldVertex v{};
            v.position[0] = t.base[0] + u[0] * crown_y + rx * ox + fx * oz;
            v.position[1] = t.base[1] + u[1] * crown_y + ry * ox + fy * oz;
            v.position[2] = t.base[2] + u[2] * crown_y + rz * ox + fz * oz;
            v.uv0[0] = (corner == 1 || corner == 2) ? t.uv[2] : t.uv[0];
            v.uv0[1] = (corner >= 2) ? t.uv[1] : crown_v_bot;
            tree_cache->cpu.push_back(v);
          }
          if (!tree_cache->batches.empty() && tree_cache->batches.back().texture == c.tex) {
            tree_cache->batches.back().vertex_count += kVertsPerShadowTree;
          } else {
            tree_cache->batches.push_back({c.tex, first_v, kVertsPerShadowTree});
          }
        }
        ++tree_cache->ver;
      }
      const uint32_t slot = r_.frame_slot();
      const VkDeviceSize region = VkDeviceSize(kMaxShadowTreeVerts) * sizeof(WorldVertex);
      tree_vb_offset = VkDeviceSize(slot) * region;
      if (tree_cache->slot_ver[slot] != tree_cache->ver && !tree_cache->cpu.empty()) {
        std::memcpy(static_cast<uint8_t*>(tree_cache->mapped) + tree_vb_offset, tree_cache->cpu.data(),
                    tree_cache->cpu.size() * sizeof(WorldVertex));
        tree_cache->slot_ver[slot] = tree_cache->ver;
      }
    }
  }

  auto draw_cascade = [&](uint32_t x_offset, VkDescriptorSet cascade_set, const ShowroomFrame& sf, bool is_far,
                          bool draw_cars) {
    const VkViewport viewport{float(x_offset), 0, float(size), float(size), 0, 1};
    const VkRect2D scissor{{int32_t(x_offset), 0}, {size, size}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float* vp = sf.view_projection;
    float clip[16];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += vp[k * 4 + r] * world_transform_[c * 4 + k];
        clip[c * 4 + r] = s;
      }
    float planes[6][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], z = clip[c * 4 + 2], w = clip[c * 4 + 3];
      planes[0][c] = w + x, planes[1][c] = w - x, planes[2][c] = w + y, planes[3][c] = w - y;
      planes[4][c] = z, planes[5][c] = w - z;
    }
    auto outside = [&](const WorldGeometry::Draw& d) {
      for (const auto& p : planes) {
        const float x = p[0] >= 0 ? d.box_max[0] : d.box_min[0];
        const float y = p[1] >= 0 ? d.box_max[1] : d.box_min[1];
        const float z = p[2] >= 0 ? d.box_max[2] : d.box_min[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return true;
      }
      return false;
    };
    world_bind_buffers(cmd, false);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &cascade_set, 0,
                            nullptr);
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    const float prop_reach = effective_near_prop_reach();
    std::vector<uint32_t> touched_textures;
    for (const auto& d : world_draws_) {
      if (!d.cull || d.blend >= 2) continue;
      const uint32_t fl = uint32_t(std::lround(d.push.params[1]));
      if ((fl & (2u | 32768u)) != 0u) continue;  // skip sky dome & water planes
      if (outside(d) || near_lod_beyond(d, eye)) continue;
      // Skip LOD02 cells wholly inside the LOD00 midpoint radius (complements near_lod_beyond).
      if ((fl & 256u) != 0u && lod_mid > prop_reach) {
        const float max_r = lod_mid - prop_reach;
        float max_d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
          const float q = std::max(std::fabs(d.box_min[k] - eye[size_t(k)]), std::fabs(d.box_max[k] - eye[size_t(k)]));
          max_d2 += q * q;
        }
        if (max_d2 < max_r * max_r) continue;
      }
      // Skip alpha-tested cutouts on mobile low-memory (avoids Adreno LRZ kill) or far cascade unless Ultra PCSS.
      if (d.blend == 1 && (!allow_shadow_cutouts || (is_far && !ultra_pcss))) continue;
      if (!allow_shadow_cutouts && is_far) {
        // On mobile low-memory, skip LOD props and small geometry in the far shadow cascade
        // (cars + buildings + terrain still cast full shadows in near & far cascades).
        if ((fl & (128u | 256u | 512u)) != 0u) continue;
        const float h_m = d.box_max[1] - d.box_min[1];
        const float w_m = std::max(d.box_max[0] - d.box_min[0], d.box_max[2] - d.box_min[2]);
        if (h_m < 4.5f && w_m < 14.0f) continue;
      }
      ++shadow_drawn_;
      const VkPipeline pipeline = d.blend == 1 ? shadow_cutout_ : shadow_world_;
      if (pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
      ShowroomPush push = d.push;
      if (d.blend == 1) {
        VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
        if (set) {
          touched_textures.push_back(d.texture);
        } else {
          set = world_white_set_;
          push.params[1] = 0;
        }
        if (set != bound_set)
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1,
                                  &(bound_set = set), 0, nullptr);
      }
      world_draw_model(d, world_transform_, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      push.params[2] = 0;
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
    if (!touched_textures.empty()) {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      for (uint32_t rec : touched_textures) world_lru_.touch(rec, world_frame_);
    }
    // Dynamic shadow casters: GPU-instanced trees, smashable debris / animated world props (balloons,
    // windmills, banners), festival crowds, and airborne showcase aircraft/balloons.
    {
      ShowroomPush dpc{};
      std::memcpy(dpc.model, world_transform_, sizeof dpc.model);
      std::memcpy(dpc.uv0, world_sun_, sizeof dpc.uv0);
      std::memcpy(dpc.uv1, world_up_, sizeof dpc.uv1);
      dpc.tint[0] = dpc.tint[1] = dpc.tint[2] = dpc.tint[3] = 1.0f;
      dpc.params[0] = 0.0f;
      dpc.params[1] = 0.0f;
      dpc.params[2] = 0.0f;
      dpc.params[3] = 1.0f;

      if (tree_cache && !tree_cache->batches.empty() && shadow_cutout_ && (!is_far || ultra_pcss)) {
        if (shadow_cutout_ != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = shadow_cutout_);
        vkCmdBindVertexBuffers(cmd, 0, 1, &tree_cache->vb.buffer, &tree_vb_offset);
        for (const auto& tb : tree_cache->batches) {
          if (tb.vertex_count == 0) continue;
          VkDescriptorSet tset = world_set(tb.texture);
          if (!tset) continue;
          if (tset != bound_set)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1,
                                    &(bound_set = tset), 0, nullptr);
          dpc.params[0] = 1.0f;
          dpc.params[1] = 1.0f;  // kFlagTextured for alpha-test in shadow.frag
          vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof dpc, &dpc);
          vkCmdDraw(cmd, tb.vertex_count, 1, tb.first_vertex, 0);
          ++shadow_drawn_;
        }
      }
      if (vfx_ready_ && debris_count_ > 0 && debris_buf_.ready()) {
        const VkDeviceSize at = dyn_sync(debris_buf_, VkDeviceSize(debris_count_) * sizeof(WorldVertex));
        vkCmdBindVertexBuffers(cmd, 0, 1, &debris_buf_.buf.buffer, &at);
        for (const auto& db : debris_batches_) {
          if (db.vertex_count == 0) continue;
          if (db.cutout && is_far && !ultra_pcss) continue;
          const VkPipeline pipe = (db.cutout && shadow_cutout_) ? shadow_cutout_ : shadow_world_;
          if (pipe != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipe);
          dpc.params[0] = db.cutout ? 1.0f : 0.0f;
          dpc.params[1] = 0.0f;
          if (db.cutout && shadow_cutout_) {
            VkDescriptorSet tset =
                (db.texture_record != kNoWorldTexture) ? world_set(db.texture_record) : VK_NULL_HANDLE;
            if (tset) {
              dpc.params[1] = 1.0f;
            } else {
              tset = world_white_set_;
            }
            if (tset && tset != bound_set)
              vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1,
                                      &(bound_set = tset), 0, nullptr);
          }
          vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof dpc, &dpc);
          vkCmdDraw(cmd, db.vertex_count, 1, db.first_vertex, 0);
          ++shadow_drawn_;
        }
      }
      if (vfx_ready_ && !is_far && spectator_count_ > 0 && spectator_buf_.ready()) {
        if (shadow_world_ != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = shadow_world_);
        dpc.params[0] = 0.0f;
        dpc.params[1] = 0.0f;
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof dpc, &dpc);
        const VkDeviceSize at = dyn_sync(spectator_buf_, VkDeviceSize(spectator_count_) * sizeof(WorldVertex));
        vkCmdBindVertexBuffers(cmd, 0, 1, &spectator_buf_.buf.buffer, &at);
        vkCmdDraw(cmd, spectator_count_, 1, 0, 0);
        ++shadow_drawn_;
      }
      if (vfx_ready_ && showcase_count_ > 0 && showcase_buf_.ready()) {
        if (shadow_world_ != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = shadow_world_);
        dpc.params[0] = 0.0f;
        dpc.params[1] = 0.0f;
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof dpc, &dpc);
        const VkDeviceSize at = dyn_sync(showcase_buf_, VkDeviceSize(showcase_count_) * sizeof(WorldVertex));
        vkCmdBindVertexBuffers(cmd, 0, 1, &showcase_buf_.buf.buffer, &at);
        vkCmdDraw(cmd, showcase_count_, 1, 0, 0);
        ++shadow_drawn_;
      }
    }
    // Player & traffic cars cast crisp shadows in Near (~25m) and Mid (~70m) cascades.
    if (draw_cars) {
      if (draw_car_) record_shadow_car(cmd, nullptr, cascade_set);
      if (!instances_.empty() && car_inst_buf_.ready() && shadow_car_) {
        std::vector<Showroom*> groups;
        for (const auto& in : instances_) {
          if (in.car && std::find(groups.begin(), groups.end(), in.car) == groups.end()) groups.push_back(in.car);
        }
        const uint32_t slot_base = r_.frame_slot() * kMaxCarInstances;
        uint32_t offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_car_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &cascade_set, 0, nullptr);
        for (Showroom* g : groups) {
          uint32_t solid_count = 0, fade_count = 0;
          float min_d2 = 1e18f;
          for (const auto& in : instances_) {
            if (in.car != g) continue;
            const float dx = in.model[12] - frame_.eye[0];
            const float dy = in.model[13] - frame_.eye[1];
            const float dz = in.model[14] - frame_.eye[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < min_d2) min_d2 = d2;
            if (in.fade_alpha >= 0.99f && offset + solid_count < kMaxCarInstances) ++solid_count;
          }
          for (const auto& in : instances_)
            if (in.car == g && in.fade_alpha < 0.99f && offset + solid_count + fade_count < kMaxCarInstances)
              ++fade_count;
#ifdef __ANDROID__
          const float max_shadow_car_d = low_memory_mode_ ? 35.0f : 50.0f;
          const bool skip_far_shadow = min_d2 > max_shadow_car_d * max_shadow_car_d;
          const auto& shadow_draws = !g->far_draws_.empty() ? g->far_draws_ : g->draws_;
#else
          const bool skip_far_shadow = min_d2 > 140.0f * 140.0f;
          const auto& shadow_draws =
              (!g->far_draws_.empty() && min_d2 > 25.0f * 25.0f) ? g->far_draws_ : g->draws_;
#endif
          if (!skip_far_shadow && solid_count > 0 && !shadow_draws.empty()) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g->vertex_buffer_.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, g->index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (const auto& d : shadow_draws) {
              if (d.glass || d.detached || d.is_blur_rim || d.cockpit_role >= 0) continue;
              const uint32_t kl = uint32_t(d.push.params[0] + 0.5f);
              if (kl == 4u || kl == 5u || kl == 8u) continue;  // kInterior, kGauge, kCaliper
              ShowroomPush push = d.push;
              push.params[1] = float(uint32_t(std::lround(push.params[1])) | 8192u);
              vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(ShowroomPush), &push);
              vkCmdDrawIndexed(cmd, d.index_count, solid_count, d.first_index, d.vertex_offset, slot_base + offset);
            }
            shadow_cars_ += solid_count;
          }
          offset += solid_count + fade_count;
        }
      }
    }
  };

  auto clear_cascade_tile = [&](uint32_t x_offset) {
    if (!partial_update) return;
    VkClearAttachment att{};
    att.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    att.clearValue.depthStencil = {1.0f, 0};
    VkClearRect rect{};
    rect.rect = {{int32_t(x_offset), 0}, {size, size}};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &att, 1, &rect);
  };

  clear_cascade_tile(0);
  draw_cascade(0, shadow_set_, shadow_frame_, false, true);
  if (cascades >= 3u && shadow_set_mid_ && shadow_set_far_ && atlas_w >= size * 3u) {
    if (shadow_update_mid_) {
      clear_cascade_tile(size);
      draw_cascade(size, shadow_set_mid_, shadow_frame_mid_, false, true);
    }
    if (shadow_update_far_) {
      clear_cascade_tile(size * 2u);
      draw_cascade(size * 2u, shadow_set_far_, shadow_frame_far_, true, false);
    }
  } else if (shadow_set_far_ && atlas_w >= size * 2u) {
    if (shadow_update_far_) {
      clear_cascade_tile(size);
      draw_cascade(size, shadow_set_far_, shadow_frame_far_, true, allow_shadow_cutouts);
    }
  }
  shadow_atlas_valid_ = true;
  shadow_atlas_world_gen_ = world_generation_;
  vkCmdEndRendering(cmd);

  b.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void Showroom::record_shadow_car(VkCommandBuffer cmd, const float* m, VkDescriptorSet cascade_set) {
  VkDescriptorSet set = cascade_set ? cascade_set : shadow_set_;
  if (draws_.empty() || !shadow_car_ || !set) return;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_car_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
  for (const auto& d : draws_) {
    if (d.glass || d.detached || d.is_blur_rim || d.cockpit_role >= 0) continue;  // skip glass, detached, blur_rim, cockpit-only parts
    const uint32_t kl = uint32_t(d.push.params[0] + 0.5f);
    if (kl == 4u || kl == 5u || kl == 8u) continue;  // kInterior, kGauge, kCaliper
    ShowroomPush push = d.push;
    if (m) multiply(m, d.push.model, push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  }
}


bool Showroom::render_mask(uint32_t width, uint32_t height, std::vector<uint8_t>& mask,
                           std::string& error, bool fraction) {
  // Drawn at the colour pass's MSAA and resolved; a pixel is covered when
  // any of its samples is, as the reference masks count every pixel the
  // car's draws change after the resolve (AUTOSHOW_REFERENCE, Mask).
  const bool msaa = r_.samples_ != VK_SAMPLE_COUNT_1_BIT;
  Renderer::Image target, multi;
  if (!r_.create_image(width, height, 1, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, target) ||
      (msaa && !r_.create_image(width, height, 1, VK_FORMAT_R8_UNORM, r_.samples_,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, multi))) {
    r_.destroy_image(target);
    r_.destroy_image(multi);
    error = "mask target";
    return false;
  }
  Renderer::Buffer readback;
  const VkDeviceSize bytes = VkDeviceSize(width) * height;
  if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback)) {
    r_.destroy_image(target);
    r_.destroy_image(multi);
    error = "mask readback";
    return false;
  }
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    update_frame(cmd);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    if (msaa) {
      b.image = multi.image;
      vkCmdPipelineBarrier2(cmd, &dep);
      b.image = target.image;
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa ? multi.view : target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0, 0, 0, 0}};
    if (msaa) {
      color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color.resolveImageView = target.view;
      color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {width, height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0, 0, float(width), float(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    record_draws(cmd, mask_, mask_);
    vkCmdEndRendering(cmd);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo host_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    host_dep.memoryBarrierCount = 1;
    host_dep.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &host_dep);
  });
  bool readback_ok = ok;
  if (ok) {
    void* mapped = nullptr;
    if (vmaMapMemory(r_.allocator_, readback.allocation, &mapped) == VK_SUCCESS) {
      const uint8_t* src = static_cast<const uint8_t*>(mapped);
      mask.resize(size_t(bytes));
      for (size_t i = 0; i < mask.size(); ++i) mask[i] = fraction ? src[i] : (src[i] != 0 ? 255 : 0);
      vmaUnmapMemory(r_.allocator_, readback.allocation);
    } else {
      error = "mask readback map failed";
      readback_ok = false;
    }
  } else {
    error = "mask render submission failed";
  }
  r_.destroy_buffer(readback);
  r_.destroy_image(target);
  r_.destroy_image(multi);
  return readback_ok;
}


}  // namespace carview
