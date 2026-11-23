#include "cars/bake.h"
#include "cars/mods.h"
#include "cars/shader_mods.h"
#include "cars/texture_mods.h"
#include "showroom.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <set>

#include "fh1data/archive.h"
#include "world/world_anim.h"
#include "fh1data/carbin.h"
#include "fh1data/xds.h"
#include "glow_frag_spv.h"
#include "glow_vert_spv.h"
#include "mask_frag_spv.h"
#include "particle_frag_spv.h"
#include "particle_vert_spv.h"
#include "shadow_frag_spv.h"
#include "showroom_frag_spv.h"
#include "showroom_vert_spv.h"
#include "contact_shadow_frag_spv.h"
#include "contact_shadow_vert_spv.h"
#include "env_prefilter_frag_spv.h"
#include "post_vert_spv.h"
#include "skid_frag_spv.h"
#include "skid_vert_spv.h"
#include "sky_frag_spv.h"
#include "sky_vert_spv.h"
#include "stars_frag_spv.h"
#include "stars_vert_spv.h"
#include "world_frag_spv.h"
#include "world_gpu_logic.h"
#include "world_opaque_frag_spv.h"
#include "world_vert_spv.h"

namespace carview {

namespace bake = cars::bake;
using bake::MaterialClass;
using bake::Role;

#define VK_TRY(expr, what)                                                       \
  do {                                                                           \
    const VkResult vk_try_result = (expr);                                       \
    if (vk_try_result != VK_SUCCESS) {                                           \
      error = std::string(what) + " failed (VkResult " +                         \
              std::to_string(int(vk_try_result)) + ")";                          \
      return false;                                                              \
    }                                                                            \
  } while (0)

namespace {

// Flags in ShowroomPush::params.y (showroom.vert / showroom.frag).
constexpr uint32_t kFlagSlot1 = 1, kFlagMetallic = 2, kFlagLivery = 8, kFlagStripe = 32, kFlagFlat = 64,
                   kFlagNoSlot0 = 16, kFlagRaceLivery = 128, kFlagGrille = 256, kFlagCutout = 512,
                   kFlagCover = 1024, kFlagLampEmissive = 2048, kFlagMirror = 4096,
                   kFlagInstanced = 8192,  // bits 14..18 carry instanced lamp channel
                   kFlagNormalMap = 524288, kFlagNormalUv1 = 1048576, kFlagDamageMap = 2097152;

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

uint16_t float_to_half(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  const uint32_t sign = (u >> 16) & 0x8000u;
  int32_t exp = int32_t((u >> 23) & 0xFFu) - 127 + 15;
  uint32_t mant = u & 0x7FFFFFu;
  if (exp <= 0) {
    if (exp < -10) return uint16_t(sign);
    mant = (mant | 0x800000u) >> (1 - exp);
    return uint16_t(sign | ((mant + 0x1000u) >> 13));
  }
  if (exp >= 31) return uint16_t(sign | 0x7BFFu);  // clamp to max finite half
  return uint16_t(sign | (uint32_t(exp) << 10) | ((mant + 0x1000u) >> 13));
}

float pack_half2x16_float(float a, float b) {
  const uint32_t packed = uint32_t(float_to_half(a)) | (uint32_t(float_to_half(b)) << 16);
  float out = 0.0f;
  std::memcpy(&out, &packed, 4);
  return out;
}

bool parse_env_u64(const char* name, uint64_t& out) {
  const char* val = std::getenv(name);
  if (!val || !*val) return false;
  while (*val && std::isspace(static_cast<unsigned char>(*val))) ++val;
  if (*val < '0' || *val > '9') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(val, &end, 10);
  if (errno == ERANGE || end == val) return false;
  while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (*end != '\0') return false;
  out = uint64_t(parsed);
  return true;
}

void identity(float* m) {
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1;
}
// Translate by t, then scale (applied first) by s.
void translate_scale(const std::array<float, 3>& t, const std::array<float, 3>& s, float* m) {
  identity(m);
  m[0] = s[0];
  m[5] = s[1];
  m[10] = s[2];
  m[12] = t[0];
  m[13] = t[1];
  m[14] = t[2];
}
// out = a b (column-major).
void multiply(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
}
std::array<float, 3> transform_point(const float* m, const std::array<float, 3>& p) {
  return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
          m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
          m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

float srgb_to_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// The model's own name for a sheet, e.g. "tyre:sidewall" with tyre textures
// "tirea" (or a car's own "tireatoy_fjcruiser_12") -> "sidewalla_clr.xds",
// and "tyre:sidewall_nrm" -> "sidewalla_nrm.xds".
std::string tyre_texture_member(const std::string& sheet, const std::string& tyre_media) {
  const char letter = tyre_media.size() > 4 ? tyre_media[4] : 'a';
  if (ends_with(sheet, "_nrm")) return sheet.substr(0, sheet.size() - 4) + letter + "_nrm.xds";
  return sheet + letter + "_clr.xds";
}

// Upgrade parts ("bumperFrace", "wingrace", "cagerace") are not drawn, and
// of a variant set ("bumperFa".."bumperFd") only the first real one, where
// `real` holds the lower-case names of parts that are not stand-ins (spec
// question Q4).
bool hidden_variant(const std::string& name, const std::set<std::string>& real) {
  const std::string n = lower(name);
  if (ends_with(n, "race")) return true;
  if (n.size() > 1) {
    const char last = n.back();
    if (last >= 'b' && last <= 'h') {
      const std::string stem = n.substr(0, n.size() - 1);
      for (char c = 'a'; c < last; ++c)
        if (real.count(stem + c)) return true;
    }
  }
  return false;
}

// FORMAT-CARS, Stock parts: the upgrade-family member a part name is or
// extends ("winga_arm1" follows "winga"), or "" when it is in no family
// (bumpers, skirts, hood and wing by letter or race, the race hoods, the
// roll cage, and the exhausts, which no table names).
std::string family_member(const std::string& name) {
  static const std::regex kVariant(
      "(bumperf|bumperr|skirtl|skirtr|hood|wing)([a-z]|race)|hood(ps|ci)race|cagerace|exhaust[lr]?[a-z]");
  const std::string n = lower(name);
  const std::string base = n.substr(0, n.find('_'));
  return std::regex_match(base, kVariant) ? base : std::string();
}

// Whether a family member is drawn for a stock row: exhausts by letter a,
// the rest when a stock row names it.
bool stock_hidden(const std::string& member, const cars::bake::StockRow& stock) {
  static const std::regex kExhaust("exhaust[lr]?([a-z])");
  std::smatch m;
  if (std::regex_match(member, m, kExhaust)) return m[1].str() != "a";
  return !std::binary_search(stock.parts.begin(), stock.parts.end(), member);
}

// Some LOD0, cockpit, brake and rim models hold stand-ins instead of
// geometry: a cube of exactly 17 vertices (0 to 0.2 m across; 506 such
// pools over the 240 cars and their wheels, while every real 17-vertex pool
// is a flat pane), a quad or triangle pair of at most 6 vertices, or a box
// of at most 24 vertices all on its bounding box's corners (one car's whole
// LOD0 body). Such pools are not drawn.
bool stub_pool(const std::vector<bake::Vertex>& pool) {
  if (pool.size() <= 6) return true;
  if (pool.size() > 24) return false;
  std::array<float, 3> lo = pool[0].position, hi = lo;
  for (const auto& v : pool)
    for (size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], v.position[k]);
      hi[k] = std::max(hi[k], v.position[k]);
    }
  const float x = hi[0] - lo[0], y = hi[1] - lo[1], z = hi[2] - lo[2];
  if (pool.size() == 17 && std::max({x, y, z}) - std::min({x, y, z}) <= 0.02f) return true;
  for (const auto& v : pool)
    for (size_t k = 0; k < 3; ++k)
      if (std::fabs(v.position[k] - lo[k]) > 0.001f && std::fabs(v.position[k] - hi[k]) > 0.001f) return false;
  return true;
}

// The part's best drawable LOD: the lowest whose pool is not a stand-in,
// or -1.
int32_t best_lod(const bake::Part& part) {
  std::set<int32_t> lods;
  for (const auto& s : part.subsections)
    if (s.material != MaterialClass::kHidden && !s.indices.empty()) lods.insert(s.lod);
  // A LOD0 pool with under a quarter of the LOD1-5 pool's vertices is a
  // stand-in too (some rims: 74 against about 2,300).
  const bool lod0_coarse = lods.count(0) && lods.size() > 1 &&
                           part.lod0_vertices.size() * 4 < part.lod_vertices.size();
  for (int32_t lod : lods) {
    if (lod == 0 && lod0_coarse) continue;
    if (!stub_pool(lod == 0 ? part.lod0_vertices : part.lod_vertices)) return lod;
  }
  return -1;
}

bool placeholder(const bake::Part& part) { return best_lod(part) < 0; }

uint32_t block_bytes(bake::TextureFormat f) {
  switch (f) {
    case bake::TextureFormat::kBc1:
    case bake::TextureFormat::kBc4: return 8;
    case bake::TextureFormat::kBc2:
    case bake::TextureFormat::kBc3:
    case bake::TextureFormat::kBc5: return 16;
    default: return 0;
  }
}
size_t level_bytes(bake::TextureFormat f, uint32_t w, uint32_t h) {
  if (f == bake::TextureFormat::kRgba8) return size_t(w) * h * 4;
  if (f == bake::TextureFormat::kR8) return size_t(w) * h;
  return size_t((w + 3) / 4) * ((h + 3) / 4) * block_bytes(f);
}
VkFormat vk_format(bake::TextureFormat f, bool srgb) {
  switch (f) {
    case bake::TextureFormat::kRgba8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case bake::TextureFormat::kBc1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case bake::TextureFormat::kBc2: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case bake::TextureFormat::kBc3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case bake::TextureFormat::kBc4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case bake::TextureFormat::kBc5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case bake::TextureFormat::kR8: return VK_FORMAT_R8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}
VkComponentSwizzle vk_swizzle(uint8_t s) {
  switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

uint16_t to_half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exponent = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = x & 0x7FFFFFu;
  if (exponent <= 0) return uint16_t(sign);
  if (exponent >= 31) return uint16_t(sign | 0x7BFFu);
  return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

// Our studio: a dark floor, a soft grey wall gradient, a large overhead
// softbox and two side strips. Linear radiance.
std::array<float, 3> studio(float x, float y, float z) {
  const float len = std::sqrt(x * x + y * y + z * z);
  x /= len;
  y /= len;
  z /= len;
  float v;
  if (y < 0) {
    v = 0.05f + 0.06f * (1 + y);  // floor, lighter towards the horizon
  } else {
    v = 0.12f + 0.35f * y;
  }
  // Overhead softbox: |x| < 0.45, |z| < 0.7 in the projection on y = 1.
  if (y > 0.5f) {
    const float px = x / y, pz = z / y;
    if (std::fabs(px) < 0.45f && std::fabs(pz) < 0.75f) v = 4.0f;
  }
  // Side strips at either side, tall and thin.
  if (std::fabs(x) > 0.85f && y > -0.1f && y < 0.6f && std::fabs(z) < 0.35f) v = std::max(v, 2.0f);
  return {v, v * 0.98f, v * 0.95f};
}

}  // namespace

Showroom::~Showroom() {
  VkDevice d = r_.device_;
  if (!d) return;
  if (!skip_wait_idle_on_destroy_) r_.wait_idle();
  rt_pipeline_.reset();
  rt_scene_.reset();
  drop_pending_world();
  drop_fading_world(true);
  stop_reaper();
  gpu_world_.reset();
  if (!guest_) r_.set_scene({});
  for (auto& t : gpu_textures_) r_.destroy_image(t);
  r_.destroy_image(custom_livery_img_);
  r_.destroy_image(environment_);
  r_.destroy_image(shared_dirt_pattern_);
  r_.destroy_image(shared_dirt_mask_);
  r_.destroy_image(shared_glass_crack_);
  r_.destroy_buffer(vertex_buffer_);
  r_.destroy_buffer(index_buffer_);
  r_.destroy_buffer(frame_buffer_);
  r_.destroy_buffer(shadow_frame_buffer_);
  r_.destroy_buffer(shadow_frame_mid_buffer_);
  r_.destroy_buffer(shadow_frame_far_buffer_);
  r_.destroy_buffer(env_frame_buffer_);
  r_.destroy_buffer(env_sky_ubo_[0]);
  r_.destroy_buffer(env_sky_ubo_[1]);
  if (env_world_opaque_) vkDestroyPipeline(d, env_world_opaque_, nullptr);
  if (env_world_coverage_) vkDestroyPipeline(d, env_world_coverage_, nullptr);
  if (env_car_opaque_) vkDestroyPipeline(d, env_car_opaque_, nullptr);
  if (env_sky_pipe_base_) vkDestroyPipeline(d, env_sky_pipe_base_, nullptr);
  if (env_sky_pipe_layers_) vkDestroyPipeline(d, env_sky_pipe_layers_, nullptr);
  if (env_prefilter_pipe_) vkDestroyPipeline(d, env_prefilter_pipe_, nullptr);
  if (env_prefilter_pipe_layout_) vkDestroyPipelineLayout(d, env_prefilter_pipe_layout_, nullptr);
  if (env_prefilter_set_layout_) vkDestroyDescriptorSetLayout(d, env_prefilter_set_layout_, nullptr);
  if (env_pool_) vkDestroyDescriptorPool(d, env_pool_, nullptr);
  if (dyn_env_mip0_cube_) vkDestroyImageView(d, dyn_env_mip0_cube_, nullptr);
  for (uint32_t m = 0; m < kDynEnvMips; ++m)
    for (int i = 0; i < 6; ++i)
      if (dyn_env_mip_faces_[m][i]) vkDestroyImageView(d, dyn_env_mip_faces_[m][i], nullptr);
  for (int i = 0; i < 6; ++i)
    if (dyn_env_faces_[i]) vkDestroyImageView(d, dyn_env_faces_[i], nullptr);
  r_.destroy_image(dyn_env_);
  r_.destroy_image(dyn_env_depth_);
  r_.destroy_image(mirror_tex_);
  r_.destroy_image(mirror_depth_);
  r_.destroy_buffer(world_vertices_);
  r_.destroy_buffer(world_indices_);
  r_.destroy_buffer(overlay_vertices_);
  for (auto& [record, t] : world_textures_) r_.destroy_image(t);
  for (VkDescriptorPool p : world_pools_) vkDestroyDescriptorPool(d, p, nullptr);
  if (sampler_) vkDestroySampler(d, sampler_, nullptr);
  if (material_pool_) vkDestroyDescriptorPool(d, material_pool_, nullptr);
  if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
  for (VkPipeline p : {opaque_, opaque_after_z_, car_z_prepass_, glass_, glass_mul_, glass_add_, mask_,
                       contact_shadow_pipe_, world_opaque_, world_opaque_after_z_, world_opaque_dither_,
                       world_z_prepass_, world_coverage_, world_alpha_, world_water_, shadow_world_, shadow_cutout_, shadow_car_,
                       sky_pipe_base_, sky_pipe_layers_, stars_pipe_})
    if (p) vkDestroyPipeline(d, p, nullptr);
  if (contact_shadow_pipe_layout_) vkDestroyPipelineLayout(d, contact_shadow_pipe_layout_, nullptr);
  if (sky_pipe_layout_) vkDestroyPipelineLayout(d, sky_pipe_layout_, nullptr);
  if (stars_pipe_layout_) vkDestroyPipelineLayout(d, stars_pipe_layout_, nullptr);
  if (sky_set_layout_) vkDestroyDescriptorSetLayout(d, sky_set_layout_, nullptr);
  if (sky_pool_) vkDestroyDescriptorPool(d, sky_pool_, nullptr);
  r_.destroy_buffer(sky_ubo_[0]);
  r_.destroy_buffer(sky_ubo_[1]);
  r_.destroy_buffer(star_vertices_);
  r_.destroy_image(sky_sun_);
  for (auto& m : sky_moons_) r_.destroy_image(m);
  r_.destroy_image(sky_dither_);
  r_.destroy_image(sky_far_f_);
  r_.destroy_image(sky_far_b_);
  r_.destroy_image(sky_far_mask_);
  r_.destroy_image(sky_close_ad_f_);
  r_.destroy_image(sky_close_ad_b_);
  r_.destroy_image(sky_close_am_f_);
  r_.destroy_image(sky_close_am_b_);
  r_.destroy_image(sky_close_tb_f_);
  r_.destroy_image(sky_close_tb_b_);
  if (glow_pipe_) vkDestroyPipeline(d, glow_pipe_, nullptr);
  if (glow_pipe_layout_) vkDestroyPipelineLayout(d, glow_pipe_layout_, nullptr);
  if (glow_set_layout_) vkDestroyDescriptorSetLayout(d, glow_set_layout_, nullptr);
  if (glow_pool_) vkDestroyDescriptorPool(d, glow_pool_, nullptr);
  r_.destroy_buffer(glow_buffer_);
  for (auto& g : glow_tex_) r_.destroy_image(g);
  r_.destroy_image(headlight_cookie_);
  r_.destroy_buffer(headlights_ubo_);
  destroy_dyn_buffer(car_inst_buf_);
  r_.destroy_buffer(empty_inst_ssbo_);
  destroy_dyn_buffer(particle_buf_);
  destroy_dyn_buffer(skid_asphalt_buf_);
  destroy_dyn_buffer(skid_rut_buf_);
  destroy_dyn_buffer(debris_buf_);
  destroy_dyn_buffer(spectator_buf_);
  r_.destroy_image(spectator_atlas_);
  spectator_set_ = VK_NULL_HANDLE;
  destroy_dyn_buffer(driver_buf_);
  r_.destroy_image(particle_atlas_);
  r_.destroy_image(skid_asphalt_tex_);
  r_.destroy_image(skid_rut_tex_);
  if (particle_alpha_pipe_) vkDestroyPipeline(d, particle_alpha_pipe_, nullptr);
  if (particle_add_pipe_) vkDestroyPipeline(d, particle_add_pipe_, nullptr);
  if (particle_pipe_layout_) vkDestroyPipelineLayout(d, particle_pipe_layout_, nullptr);
  if (particle_set_layout_) vkDestroyDescriptorSetLayout(d, particle_set_layout_, nullptr);
  if (particle_pool_) vkDestroyDescriptorPool(d, particle_pool_, nullptr);
  if (particle_sampler_) vkDestroySampler(d, particle_sampler_, nullptr);
  if (skid_pipe_) vkDestroyPipeline(d, skid_pipe_, nullptr);
  if (skid_pipe_layout_) vkDestroyPipelineLayout(d, skid_pipe_layout_, nullptr);
  if (skid_set_layout_) vkDestroyDescriptorSetLayout(d, skid_set_layout_, nullptr);
  if (skid_pool_) vkDestroyDescriptorPool(d, skid_pool_, nullptr);
  if (skid_sampler_) vkDestroySampler(d, skid_sampler_, nullptr);
  r_.destroy_image(flat_normal_tex_);
  if (world_pipeline_layout_) vkDestroyPipelineLayout(d, world_pipeline_layout_, nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
  if (frame_layout_) vkDestroyDescriptorSetLayout(d, frame_layout_, nullptr);
  if (world_material_layout_) vkDestroyDescriptorSetLayout(d, world_material_layout_, nullptr);
  if (material_layout_) vkDestroyDescriptorSetLayout(d, material_layout_, nullptr);
}

bool Showroom::load(const std::filesystem::path& bake_dir, const std::string& car,
                    const ShowroomOptions& options, std::string& error) {
  car_ = {};
  wheel_ = {};
  tyre_ = {};
  tyre_textures_ = {};
  // Keep shared_ loaded across hot car reloads when bake_dir hasn't changed.
  if (shared_bake_dir_ != bake_dir) {
    shared_ = {};
    shared_bake_dir_.clear();
  }
  vertices_.clear();
  indices_.clear();
  draws_.clear();
  textures_.clear();
  texture_by_key_.clear();
  descriptor_by_textures_.clear();
  drawn_parts_.clear();
  skipped_parts_.clear();
  notes_.clear();
  have_bounds_ = false;
  bounds_min_ = {};
  bounds_max_ = {};
  for (auto& rig : cockpit_rigs_) rig = {};
  stock_ = nullptr;
  pending_wheel_ = -1;
  pending_spins_ = false;
  for (int i = 0; i < 4; ++i) {
    stripe_[i] = 1.0f;
    paint_two_tone_[i] = 0.0f;
    damage_dirt_[i] = 0.0f;
    damage_zones_[i] = 0.0f;
  }
  paint_finish_[0] = 0.0f;
  paint_finish_[1] = 0.25f;
  paint_finish_[2] = 0.0f;
  paint_finish_[3] = 0.0f;
  options_ = options;
  std::string err;
  const std::string car_name = lower(car);
  if (!bake::read_file(cars::mods::resolve(bake_dir / "cars" / (car_name + ".shiftbake")), car_, &err)) {
    error = car_name + ": " + err;
    return false;
  }
  bake::CarInfo& info = car_.car;
  if (options_.match_rgb >= 0) {
    bool found = false;
    for (size_t i = 0; i < info.colours.size() && !found; ++i)
      if (int64_t(info.colours[i].rgb) == options_.match_rgb) {
        options_.colour = i;
        found = true;
      }
    if (!found) notes_.push_back("no colour row with the requested sRGB: drawing colour " +
                                 std::to_string(options_.colour));
  }
  if (options_.match_sequence >= 0) {
    bool found = false;
    for (size_t i = 0; i < info.colours.size() && !found; ++i)
      if (info.colours[i].sequence == options_.match_sequence) {
        options_.colour = i;
        found = true;
      }
    if (!found) {
      options_.colour = 0;
      notes_.push_back("no colour with Sequence " + std::to_string(options_.match_sequence) + ": the lowest");
    }
  }

  // Apply fitted visual overrides (tyre widths, rim diameters, ride height,
  // CarTrackOffsets, caliper angles, aftermarket rim, per-car tyre).
  const cars::VisualSetup& vis = options_.visual;
  if (vis.active) {
    for (size_t a = 0; a < 2; ++a) {
      if (vis.tyre_width_mm[a] > 0) info.tyre_width_mm[a] = vis.tyre_width_mm[a];
      if (vis.rim_inches[a] > 0) info.rim_inches[a] = vis.rim_inches[a];
      if (vis.ride_height_m[a] >= 0.0f) info.ride_height[a] = vis.ride_height_m[a];
      if (vis.has_caliper_angle_override) info.caliper_angle[a] = vis.caliper_angle_deg[a];
    }
    if (!vis.wheel_media.empty()) info.wheel_media = lower(vis.wheel_media);
    // Recompute lift, pitch, and wheel centres for upgraded stance / track offsets.
    if (info.has_body && info.wheelbase > 0.0f) {
      info.lift = (info.ride_height[0] + info.ride_height[1]) * 0.5f - info.body_bottom_y;
      info.pitch = std::atan((info.ride_height[1] - info.ride_height[0]) / info.wheelbase);
      const float cs = std::cos(info.pitch), sn = std::sin(info.pitch);
      const size_t wheel_rows[4] = {5, 6, 8, 7};
      for (size_t w = 0; w < 4; ++w) {
        const size_t axle = w < 2 ? 0 : 1;
        const float rim_r = float(info.rim_inches[axle]) * 0.0254f * 0.5f;
        const float width = float(info.tyre_width_mm[axle]) / 1000.0f;
        const float outer = rim_r + width * float(info.tyre_aspect[axle]) / 100.0f;
        const float side = (w % 2 == 0) ? -1.0f : 1.0f;
        const float track_x = (info.track_outer[axle] - width) * 0.5f + vis.track_offset_m[axle];
        const float bx = side * track_x;
        const float by = outer;
        const float bz = info.part_positions[wheel_rows[w]][2];
        info.wheel_centres_body[w] = {bx, by, bz};
        const float y = by - info.lift;
        info.wheel_centres[w] = {bx, cs * y + sn * bz, sn * y - cs * bz};
      }
    }
  }

  // Per-car tyre check (FJ Cruiser TOY_FJCruiser_12, Raptor, Wrangler, H1):
  // prefer the car's own TireA<MediaName> archive if available in bake or disc.
  std::string want_tyre = vis.active && !vis.tyre_media_override.empty()
                              ? lower(vis.tyre_media_override)
                              : cars::per_car_tyre_media(info.media_name);
  if (!want_tyre.empty() && info.tyre_media == cars::kTyreModel) {
    if (std::filesystem::exists(cars::mods::resolve(bake_dir / "wheels" / (want_tyre + ".shiftbake")))) {
      info.tyre_media = want_tyre;
      info.tyre_texture_media = want_tyre;
    } else if (!options_.game_root.empty() &&
               cars::load_wheel_from_disc(options_.game_root, want_tyre, true, tyre_, nullptr)) {
      info.tyre_media = want_tyre;
      info.tyre_texture_media = want_tyre;
      tyre_textures_ = tyre_;
    }
  }

  // Load rim: pre-baked .shiftbake first, then on-demand from media/wheels/<MediaName>.zip
  // (covers all 285+ aftermarket rims in List_Wheels without requiring pre-baking).
  if (!bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (info.wheel_media + ".shiftbake")), wheel_, &err)) {
    std::string disc_err;
    if (options_.game_root.empty() ||
        !cars::load_wheel_from_disc(options_.game_root, info.wheel_media, false, wheel_, &disc_err)) {
      error = "wheel " + info.wheel_media + ": " + err + (disc_err.empty() ? "" : " (" + disc_err + ")");
      return false;
    }
  }
  if (tyre_.models.empty()) {
    if (!bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (info.tyre_media + ".shiftbake")), tyre_, &err)) {
      std::string disc_err;
      if (options_.game_root.empty() ||
          !cars::load_wheel_from_disc(options_.game_root, info.tyre_media, true, tyre_, &disc_err)) {
        error = err;
        return false;
      }
    }
  }
  if (tyre_textures_.textures.empty()) {
    if (!bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (info.tyre_texture_media + ".shiftbake")),
                         tyre_textures_, &err)) {
      if (!bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (std::string(cars::kTyreTextures) + ".shiftbake")),
                           tyre_textures_, &err)) {
        error = err;
        return false;
      }
    }
  }
  if (shared_.textures.empty()) {
    if (!bake::read_file(cars::mods::resolve(bake_dir / "shared.shiftbake"), shared_, &err)) {
      error = err;
      return false;
    }
    shared_bake_dir_ = bake_dir;
  }

  // On-demand load of car interior_emissive_LOD0.xds (real backlit speedometer/tachometer dials)
  // when not already stored in pre-baked .shiftbake packs.
  {
    bool has_emissive = false;
    for (const auto& tx : car_.textures) {
      if (lower(tx.name) == "interior_emissive_lod0.xds") {
        has_emissive = true;
        break;
      }
    }
    if (!has_emissive && !options_.game_root.empty()) {
      const std::filesystem::path cars_dir = options_.game_root / "media" / "cars";
      std::filesystem::path zip_path;
      std::error_code ec;
      const std::string want_zip = lower(info.media_name) + ".zip";
      if (std::filesystem::is_directory(cars_dir, ec)) {
        for (const auto& ent : std::filesystem::directory_iterator(cars_dir, ec)) {
          if (ent.is_regular_file(ec) && lower(ent.path().filename().string()) == want_zip) {
            zip_path = ent.path();
            break;
          }
        }
      }
      if (!zip_path.empty()) {
        if (auto arch = fh1::Archive::open(zip_path, nullptr)) {
          auto rd = arch->read("interior_emissive_lod0.xds");
          if (rd.ok()) {
            auto xds = fh1::decode_xds(rd.data);
            if (!xds.levels.empty() && !xds.levels[0].rgba.empty()) {
              bake::Texture bt;
              bt.name = "interior_emissive_lod0.xds";
              bt.format = bake::TextureFormat::kRgba8;
              bt.levels.push_back({xds.levels[0].width, xds.levels[0].height, std::move(xds.levels[0].rgba)});
              car_.textures.push_back(std::move(bt));
            }
          }
        }
      }
    }
  }

  textures_.push_back({nullptr, "@white"});
  textures_.push_back({nullptr, "@black"});

  // The row shown: the requested car id, else the baked row.
  stock_ = nullptr;
  for (const auto& s : info.stock_rows)
    if (s.known && (options_.car_id == 0 ? s.car_id == info.car_id : s.car_id == options_.car_id)) stock_ = &s;
  if (options_.car_id != 0 && !stock_)
    notes_.push_back("no stock row for car " + std::to_string(options_.car_id) + " in the bake");
  if (!stock_ && !vis.active) notes_.push_back("no stock parts baked: hiding variants by letter (first real letter)");
  // FORMAT-CARS, Stock & fitted parts: family members by fitted VisualSetup
  // (when active) else the stock rows, seats a weight row removes, and
  // other lettered names by the first real letter.
  auto hidden = [&](const std::string& name, const std::set<std::string>& real) {
    if (options_.all_variants) return false;
    const std::string n = lower(name);
    if (vis.active) {
      const std::string member = family_member(n);
      if (!member.empty()) {
        static const std::regex kExhaust("exhaust[lr]?([a-z])");
        std::smatch m;
        if (std::regex_match(member, m, kExhaust)) {
          if (vis.has_exhaust_override) {
            return !std::binary_search(vis.shown_parts.begin(), vis.shown_parts.end(), member);
          }
          return m[1].str() != "a";
        }
        return !std::binary_search(vis.shown_parts.begin(), vis.shown_parts.end(), member);
      }
      if (std::binary_search(vis.removed_parts.begin(), vis.removed_parts.end(), n)) return true;
      return hidden_variant(n, real);
    }
    if (stock_) {
      const std::string member = family_member(n);
      if (!member.empty()) return stock_hidden(member, *stock_);
      if (std::binary_search(stock_->removed.begin(), stock_->removed.end(), n)) return true;
    }
    return hidden_variant(n, real);
  };

  // Which body: LOD0 when it has geometry, else the LOD1-5 model.
  const bake::Model* body = nullptr;
  const bake::Model* exterior = nullptr;
  const bake::Model* cockpit = nullptr;
  for (const auto& m : car_.models) {
    bool real = false;
    for (const auto& p : m.parts) real |= !placeholder(p);
    if (m.role == Role::kLod0 && real) body = &m;
    if (m.role == Role::kExterior) exterior = &m;
    if (m.role == Role::kCockpit) cockpit = &m;
  }
  if (!body) {
    body = exterior;
    notes_.push_back("no LOD0 geometry (or only stand-in boxes): drawing the LOD1-5 model");
  }
  if (!body) {
    error = car_name + ": no exterior model";
    return false;
  }

  float identity_m[16];
  identity(identity_m);
  std::set<std::string> body_names, body_real;
  for (const auto& p : body->parts) {
    body_names.insert(lower(p.name));
    if (!placeholder(p)) body_real.insert(lower(p.name));
  }
  std::vector<std::string> body_parts;
  for (const auto& p : body->parts) {
    const std::string n = lower(p.name);
    // The LOD0 model carries a rim ("wheel") at the origin; the wheels are
    // drawn from the wheel model at each centre instead.
    if (n == "wheel" || placeholder(p) || hidden(n, body_real)) {
      skipped_parts_.push_back(body->name + "/" + p.name);
      continue;
    }
    body_parts.push_back(p.name);
  }
  add_model(*body, body_parts, identity_m, car_, 0);
  // The cockpit model: the parts the body does not have (hood, windscreen,
  // headlights, cabin); the gauge needles at their minimum.
  std::set<std::string> cockpit_real;
  if (cockpit)
    for (const auto& p : cockpit->parts)
      if (!placeholder(p)) cockpit_real.insert(lower(p.name));
  if (!cockpit_real.empty()) {
    // Extract living-cockpit rotation rigs for steering_wheel and analog gauge needles (_min/_mid/_max).
    auto pool_of = [&](const bake::Part& p) -> const std::vector<bake::Vertex>* {
      const int32_t b = best_lod(p);
      if (b < 0) return nullptr;
      return b == 0 ? &p.lod0_vertices : &p.lod_vertices;
    };
    auto find_part = [&](const std::string& lname) -> const bake::Part* {
      for (const auto& p : cockpit->parts)
        if (lower(p.name) == lname && !placeholder(p)) return &p;
      return nullptr;
    };
    auto vsub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    auto vdot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    auto vcross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto vnorm = [&](const std::array<float, 3>& a) -> std::array<float, 3> {
      const float l = std::sqrt(vdot(a, a));
      return l > 1e-6f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : std::array<float, 3>{0, 0, -1};
    };
    if (const bake::Part* sw = find_part("steering_wheel")) {
      if (const auto* pool = pool_of(*sw); pool && pool->size() >= 3) {
        std::array<double, 3> mean{0, 0, 0};
        for (const auto& v : *pool)
          for (int k = 0; k < 3; ++k) mean[k] += v.position[k];
        for (int k = 0; k < 3; ++k) mean[k] /= double(pool->size());
        // Steering column axis: normal to wheel rim plane (smallest-variance axis in YZ plane).
        double syy = 0, syz = 0, szz = 0;
        for (const auto& v : *pool) {
          const double dy = v.position[1] - mean[1], dz = v.position[2] - mean[2];
          syy += dy * dy, syz += dy * dz, szz += dz * dz;
        }
        const double theta = 0.5 * std::atan2(2.0 * syz, syy - szz);
        // Eigenvector with smaller variance in YZ plane.
        const double ay1 = std::cos(theta), az1 = std::sin(theta);
        const double ay2 = -az1, az2 = ay1;
        const double v1 = ay1 * ay1 * syy + 2.0 * ay1 * az1 * syz + az1 * az1 * szz;
        const double v2 = ay2 * ay2 * syy + 2.0 * ay2 * az2 * syz + az2 * az2 * szz;
        float ay = float(v1 < v2 ? ay1 : ay2), az = float(v1 < v2 ? az1 : az2);
        // Orient forward (-Z in model space) so +wheel_rad rotates top of wheel toward +X (right).
        if (az > 0.0f) ay = -ay, az = -az;
        cockpit_rigs_[0].valid = true;
        cockpit_rigs_[0].pivot = {float(mean[0]), float(mean[1]), float(mean[2])};
        cockpit_rigs_[0].axis = vnorm({0.0f, ay, az});
      }
    }
    static const char* const kGauges[5] = {"speed", "tach", "fuel", "boost", "power"};
    for (int g = 0; g < 5; ++g) {
      const bake::Part* pmin = find_part(std::string(kGauges[g]) + "_min");
      const bake::Part* pmid = find_part(std::string(kGauges[g]) + "_mid");
      const bake::Part* pmax = find_part(std::string(kGauges[g]) + "_max");
      if (!pmin || !pmax) continue;
      const auto* v0 = pool_of(*pmin);
      const auto* v1 = pmid ? pool_of(*pmid) : nullptr;
      const auto* v2 = pool_of(*pmax);
      if (!v0 || !v2 || v0->empty() || v2->empty()) continue;
      auto& rig = cockpit_rigs_[g + 1];
      if (v0->size() == v2->size()) {
        size_t i_piv = 0, i_tip = 0;
        float d_min = 1e30f, d_max = -1.0f;
        for (size_t i = 0; i < v0->size(); ++i) {
          const auto diff = vsub((*v2)[i].position, (*v0)[i].position);
          const float d2 = vdot(diff, diff);
          if (d2 < d_min) d_min = d2, i_piv = i;
          if (d2 > d_max) d_max = d2, i_tip = i;
        }
        if (d_max > 1e-6f) {
          const auto piv = (*v0)[i_piv].position;
          const auto u0 = vnorm(vsub((*v0)[i_tip].position, piv));
          const auto u2 = vnorm(vsub((*v2)[i_tip].position, piv));
          if (v1 && v1->size() == v0->size()) {
            const auto u1 = vnorm(vsub((*v1)[i_tip].position, piv));
            auto ax = vcross(u0, u1);
            if (vdot(ax, ax) < 1e-6f) ax = vcross(u0, u2);
            if (vdot(ax, ax) > 1e-6f) {
              ax = vnorm(ax);
              float a1 = std::atan2(vdot(vcross(u0, u1), ax), vdot(u0, u1));
              if (a1 < 0.0f) a1 += 6.2831853f;
              float a2 = std::atan2(vdot(vcross(u1, u2), ax), vdot(u1, u2));
              if (a2 < 0.0f) a2 += 6.2831853f;
              rig.valid = true;
              rig.pivot = piv;
              rig.axis = ax;
              rig.ang_mid = a1;
              rig.ang_max = a1 + a2;
            }
          } else {
            auto ax = vcross(u0, u2);
            if (vdot(ax, ax) > 1e-6f) {
              ax = vnorm(ax);
              float am = std::atan2(vdot(vcross(u0, u2), ax), vdot(u0, u2));
              if (am < 0.0f) am += 6.2831853f;
              rig.valid = true;
              rig.pivot = piv;
              rig.axis = ax;
              rig.ang_mid = am * 0.5f;
              rig.ang_max = am;
            }
          }
        }
      }
    }

    std::vector<std::string> cockpit_parts;
    for (const auto& p : cockpit->parts) {
      const std::string n = lower(p.name);
      const bool needle = ends_with(n, "_mid") || ends_with(n, "_max");
      if (body_names.count(n) || needle || placeholder(p) || hidden(n, cockpit_real)) {
        skipped_parts_.push_back(cockpit->name + "/" + p.name);
        continue;
      }
      cockpit_parts.push_back(p.name);
    }
    add_model(*cockpit, cockpit_parts, identity_m, car_, 0);
  } else {
    notes_.push_back("no cockpit geometry: interior from the body model only");
  }

  // Stand-in parts of LOD0 and the cockpit come from the LOD1-5 model,
  // unless their variant set has a real part already.
  if (exterior && body != exterior) {
    std::set<std::string> stand_ins;
    for (const auto& p : body->parts)
      if (placeholder(p)) stand_ins.insert(lower(p.name));
    if (cockpit)
      for (const auto& p : cockpit->parts)
        if (placeholder(p) && !body_names.count(lower(p.name))) stand_ins.insert(lower(p.name));
    std::set<std::string> real = body_real;
    real.insert(cockpit_real.begin(), cockpit_real.end());
    auto stem_of = [](const std::string& n) {
      return n.size() > 1 && n.back() >= 'a' && n.back() <= 'h' ? n.substr(0, n.size() - 1) : std::string();
    };
    std::set<std::string> stems_drawn;
    for (const auto& n : real)
      if (!stem_of(n).empty()) stems_drawn.insert(stem_of(n));
    std::vector<std::string> fill;
    for (const auto& n : stand_ins) {  // sorted: "bumperfa" before "bumperfb"
      if (n == "wheel" || ends_with(n, "_mid") || ends_with(n, "_max")) continue;
      // A stock/fitted family member is filled from its LOD1-5 twin; another
      // member never stands in for it (no promotion).
      const bool by_stock = (stock_ || vis.active) && !family_member(n).empty();
      if (by_stock && hidden(n, real)) continue;
      if (!by_stock && ends_with(n, "race")) continue;
      if (!by_stock && (stock_ || vis.active) && hidden(n, real)) continue;  // a removed seat
      const std::string stem = by_stock ? std::string() : stem_of(n);
      if (!options_.all_variants && !stem.empty() && stems_drawn.count(stem)) continue;
      for (const auto& e : exterior->parts)
        if (lower(e.name) == n && !placeholder(e)) {
          fill.push_back(e.name);
          if (!stem.empty()) stems_drawn.insert(stem);
        }
    }
    if (!fill.empty()) {
      notes_.push_back(std::to_string(fill.size()) + " stand-in parts drawn from the LOD1-5 model");
      add_model(*exterior, fill, identity_m, car_, 0);
    }
  }

  // The inverse pose: model = R (body - (0, lift, 0)); R is its own inverse.
  const float cp = std::cos(info.pitch), sp = std::sin(info.pitch);
  const float pose[16] = {1, 0, 0, 0, 0, cp, sp, 0, 0, sp, -cp, 0, 0, -cp * info.lift, -sp * info.lift, 1};
  std::memcpy(body_to_model_, pose, sizeof pose);
  const float* body_to_model = body_to_model_;

  // Brakes: each corner's rotor and calliper, centred on the hub, rotated by
  // CaliperAngleFront/Rear and laterally offset by List_Wheels.BrakeDrawOffset.
  // When upgraded brakes (Level >= 1) are fitted and game_root is set, load
  // the upgraded rotor & caliper models from media/brakes/.
  static constexpr bake::Corner kCorners[4] = {bake::Corner::kLF, bake::Corner::kRF, bake::Corner::kLR,
                                               bake::Corner::kRR};
  constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
  std::array<bake::Model, 2> upg_rotor, upg_caliper;
  std::array<bool, 2> has_upg_rotor{false, false}, has_upg_caliper{false, false};
  if (vis.active && !options_.game_root.empty()) {
    for (int a = 0; a < 2; ++a) {
      if (vis.brakes[size_t(a)].level >= 1 && vis.brakes[size_t(a)].size_in > 0) {
        has_upg_rotor[size_t(a)] =
            cars::load_brake_model(options_.game_root, vis.brakes[size_t(a)].size_in, vis.brakes[size_t(a)].type_id,
                                   vis.brakes[size_t(a)].pistons, true, upg_rotor[size_t(a)], nullptr);
        has_upg_caliper[size_t(a)] =
            cars::load_brake_model(options_.game_root, vis.brakes[size_t(a)].size_in, vis.brakes[size_t(a)].type_id,
                                   vis.brakes[size_t(a)].pistons, false, upg_caliper[size_t(a)], nullptr);
      }
    }
  }
  auto add_brake_at = [&](const bake::Model& m, int corner, bool is_rotor, bool mirror_right,
                          const bake::Bake& tex_pack) {
    const int axle = corner < 2 ? 0 : 1;
    const bool right = (corner % 2) == 1;
    const float side = right ? 1.0f : -1.0f;
    const float brake_dx = vis.active ? (side * vis.brake_draw_offset) : 0.0f;
    const float cal_rad = is_rotor ? 0.0f : (info.caliper_angle[size_t(axle)] * kDegToRad);
    const float ca = std::cos(cal_rad), sa = std::sin(cal_rad);
    // Local transform about hub in body frame: optional half-turn for loose
    // right-side models, caliper angle rotation around axle X, lateral offset.
    float rot_x[16];
    identity(rot_x);
    rot_x[5] = ca;
    rot_x[6] = sa;
    rot_x[9] = -sa;
    rot_x[10] = ca;
    float local_body[16];
    if (mirror_right) {
      float flip[16];
      identity(flip);
      flip[0] = right ? -1.0f : 1.0f;
      flip[10] = right ? -1.0f : 1.0f;
      multiply(rot_x, flip, local_body);
    } else {
      // Stock brake models are authored in model frame: convert linear part via pose.
      float lin_pose[16];
      std::memcpy(lin_pose, pose, sizeof lin_pose);
      lin_pose[13] = lin_pose[14] = 0;
      multiply(rot_x, lin_pose, local_body);
    }
    local_body[12] += brake_dx;
    std::memcpy(pending_local_, local_body, sizeof local_body);
    pending_wheel_ = corner;
    pending_spins_ = is_rotor;
    const auto& cb = info.wheel_centres_body[size_t(corner)];
    local_body[12] += cb[0];
    local_body[13] = cb[1];
    local_body[14] = cb[2];
    float m4[16];
    multiply(body_to_model, local_body, m4);
    std::vector<std::string> parts;
    for (const auto& p : m.parts) parts.push_back(p.name);
    add_model(m, parts, m4, tex_pack, axle);
  };
  for (int c = 0; c < 4; ++c) {
    const size_t axle = c < 2 ? 0 : 1;
    if (has_upg_rotor[axle]) add_brake_at(upg_rotor[axle], c, true, true, car_);
    if (has_upg_caliper[axle]) add_brake_at(upg_caliper[axle], c, false, true, car_);
  }
  for (const auto& m : car_.models) {
    if (m.role != Role::kCaliper && m.role != Role::kRotor) continue;
    int corner = -1;
    for (int c = 0; c < 4; ++c)
      if (m.corner == kCorners[c]) corner = c;
    if (corner < 0) {
      notes_.push_back(m.name + ": no corner");
      continue;
    }
    const size_t axle = corner < 2 ? 0 : 1;
    if (m.role == Role::kRotor && has_upg_rotor[axle]) continue;
    if (m.role == Role::kCaliper && has_upg_caliper[axle]) continue;
    add_brake_at(m, corner, m.role == Role::kRotor, false, car_);
  }
  pending_wheel_ = -1;

  // Wheels (FORMAT-CARS, Wheels): built in the body frame at each centre and
  // mapped to the model frame by the inverse of the model-to-body pose.
  // Rims are sized by their morph target, never scaled radially; tyres by
  // their two targets; both are scaled across from the rim header's design
  // width. Right wheels are the same models turned half a turn about y.
  const bake::Model* rim = nullptr;
  for (const auto& m : wheel_.models)
    if (m.role == Role::kWheel) rim = &m;
  const bake::Model* tyre = nullptr;
  for (const auto& m : tyre_.models)
    if (m.role == Role::kTyre) tyre = &m;
  // A model's own tyre set (FORMAT-CARS, Wheels) is one unit wide like
  // the shared tyre and carries the same two targets (on the FJ Cruiser and
  // the Raptor-sized sets target A is the modelled radius, on the Wrangler
  // it grows the tyre): ours, until its sizing is settled, it is sized as
  // the shared tyre is (spec question Q39).
  if (info.tyre_media.size() > 5) notes_.push_back("own tyre set " + info.tyre_media + ": sized as the shared tyre");
  bool header_ok = false, extent_ok = false;
  if (rim) {
    const auto& hb = rim->header_bounds;
    auto finite = [](float f) { return std::isfinite(f) && std::fabs(f) < 1e3f; };
    header_ok = rim->header_kind == 3 && finite(hb[1]) && finite(hb[3]) && hb[1] - hb[3] > 0.01f;
    extent_ok = header_ok && finite(hb[0]) && finite(hb[2]) && hb[0] - hb[2] > 0.01f;
    if (!header_ok) notes_.push_back("rim header design width not finite: rim drawn unscaled across");
    else if (!extent_ok)
      notes_.push_back("rim header x extent not finite: tyre fitted to the design width (spec question Q13)");
    bool morph = false, lod0 = false;
    for (const auto& p : rim->parts) {
      const int32_t lod = best_lod(p);
      if (lod == 0) lod0 = true;
      if (!(lod == 0 ? p.lod0_morphs : p.lod_morphs).empty()) morph = true;
    }
    if (!morph) notes_.push_back("rim has no morph target: drawn as authored");
    if (!lod0) notes_.push_back("rim drawn from LOD1 (its LOD0 pool is missing or a stand-in)");
  }
  // Morph a copy of a wheel model: each drawn pool's targets at the weights.
  auto morphed = [](const bake::Model& model, const std::array<float, 2>& weights) {
    bake::Model out = model;
    for (auto& p : out.parts) {
      for (int lod = 0; lod < 2; ++lod) {
        auto& pool = lod == 0 ? p.lod0_vertices : p.lod_vertices;
        const auto& targets = lod == 0 ? p.lod0_morphs : p.lod_morphs;
        std::vector<std::array<float, 3>> base(pool.size());
        for (size_t i = 0; i < pool.size(); ++i) base[i] = pool[i].position;
        for (size_t t = 0; t < targets.size() && t < 2; ++t) {
          if (weights[t] == 0 || targets[t].size() != pool.size()) continue;
          for (size_t i = 0; i < pool.size(); ++i)
            for (size_t k = 0; k < 3; ++k)
              pool[i].position[k] += weights[t] * (targets[t][i][k] - base[i][k]);
        }
      }
      p.lod0_morphs.clear();
      p.lod_morphs.clear();
    }
    return out;
  };
  for (int c = 0; c < 4; ++c) {
    const int axle = c < 2 ? 0 : 1;
    const bool right = c % 2 == 1;
    const float inches = info.rim_inches[size_t(axle)] > 0 ? float(info.rim_inches[size_t(axle)]) : 14.0f;
    const float rim_radius = inches * 0.0254f / 2;
    const float width_m = info.tyre_width_mm[size_t(axle)] > 0
                              ? float(info.tyre_width_mm[size_t(axle)]) / 1000.0f
                              : 0.2f;
    const float aspect = info.tyre_aspect[size_t(axle)] > 0 ? float(info.tyre_aspect[size_t(axle)]) / 100.0f
                                                           : 0.5f;
    const float outer = rim_radius + width_m * aspect;
    const float w_rim = (inches - 14.0f) / 10.0f;
    const float rim_scale = rim && header_ok ? width_m / (rim->header_bounds[1] - rim->header_bounds[3]) : 1.0f;
    // Body-frame wheel matrix: centre, static alignment camber about Z, the
    // half turn for right wheels, then the lateral scale (and the tyre's
    // lateral offset in the rim's x).
    const float cam = vis.active ? (right ? -vis.camber_rad[size_t(axle)] : vis.camber_rad[size_t(axle)]) : 0.0f;
    const float cc = std::cos(cam), sc = std::sin(cam);
    auto wheel_matrix = [&](float sx, float dx, float* m4) {
      float base_m[16];
      identity(base_m);
      const float turn = right ? -1.0f : 1.0f;
      base_m[0] = turn * sx;
      base_m[10] = turn;
      base_m[12] = turn * dx;
      float local[16];
      if (cam != 0.0f) {
        float rz[16];
        identity(rz);
        rz[0] = cc;
        rz[1] = sc;
        rz[4] = -sc;
        rz[5] = cc;
        multiply(rz, base_m, local);
      } else {
        std::memcpy(local, base_m, sizeof local);
      }
      const auto& cb = info.wheel_centres_body[size_t(c)];
      std::memcpy(pending_local_, local, sizeof local);  // about the centre (set_wheel_poses)
      pending_wheel_ = c;
      pending_spins_ = true;
      local[12] += cb[0];
      local[13] += cb[1];
      local[14] += cb[2];
      multiply(body_to_model, local, m4);
    };
    if (rim) {
      float m4[16];
      wheel_matrix(rim_scale, 0, m4);
      const bake::Model sized = morphed(*rim, {w_rim, 0});
      std::vector<std::string> parts;
      for (const auto& p : sized.parts) parts.push_back(p.name);
      add_model(sized, parts, m4, wheel_, axle);
    }
    if (tyre) {
      // Target A raises the outer radius, target B the inner one.
      const float w_outer = (outer - 0.2850f) / 0.1301f;
      // The rim's x extent (bytes 32 and 40), or its design faces (36 and 44)
      // where the extent is empty.
      const auto& hb = rim ? rim->header_bounds : std::array<float, 4>{};
      const float extent = extent_ok ? hb[0] - hb[2] : header_ok ? hb[1] - hb[3] : width_m;
      const float centre = extent_ok ? (hb[0] + hb[2]) / 2 : header_ok ? (hb[1] + hb[3]) / 2 : 0.0f;
      float m4[16];
      wheel_matrix(rim_scale * extent, rim_scale * centre, m4);
      const bake::Model sized = morphed(*tyre, {w_outer, w_rim});
      std::vector<std::string> parts;
      for (const auto& p : sized.parts) parts.push_back(p.name);
      add_model(sized, parts, m4, tyre_, axle);
    }
  }
  pending_wheel_ = -1;
  if (!rim) notes_.push_back("no rim model in " + info.wheel_media);
  if (!tyre) notes_.push_back("no tyre model in " + info.tyre_media);
  if (draws_.empty()) {
    error = car_name + ": nothing to draw";
    return false;
  }
  return true;
}

uint32_t Showroom::texture_index(const std::string& slot, const bake::Bake& pack, bool lod0) {
  if (slot.empty()) return UINT32_MAX;
  const bake::Bake* source = nullptr;
  std::string member, key;
  if (slot == "@atlas") {
    const auto& colours = car_.car.colours;
    if (colours.empty()) return UINT32_MAX;
    const auto& colour = colours[std::min(options_.colour, colours.size() - 1)];
    member = lod0 && !colour.atlas_lod0.empty() ? colour.atlas_lod0 : colour.atlas_lod;
    if (member.empty()) member = colour.atlas_lod0;
    source = &car_;
    key = "car:" + member;
  } else if (slot.rfind("car:", 0) == 0) {
    member = slot.substr(4);
    source = &car_;
    key = slot;
  } else if (slot.rfind("wheel:", 0) == 0) {
    member = slot.substr(6);
    source = &pack;
    key = (&pack == &wheel_ ? "wheel:" : "tyre-model:") + member;
  } else if (slot.rfind("tyre:", 0) == 0) {
    member = tyre_texture_member(slot.substr(5), car_.car.tyre_texture_media);
    source = &tyre_textures_;
    key = "tyre:" + member;
  } else if (slot.rfind("shared:", 0) == 0) {
    member = slot.substr(7);
    source = &shared_;
    key = slot;
  } else if (slot == "livery_mask") {
    member = slot;
    source = &car_;
    key = "car:livery_mask";
  }
  if (!source) return UINT32_MAX;
  if (auto it = texture_by_key_.find(key); it != texture_by_key_.end()) return it->second;
  const bake::Texture* found = nullptr;
  for (const auto& t : source->textures)
    if (t.name == member) found = &t;
  if (!found) {
    notes_.push_back("missing texture " + key);
    texture_by_key_[key] = UINT32_MAX;
    return UINT32_MAX;
  }
  const uint32_t index = uint32_t(textures_.size());
  textures_.push_back({found, key});
  texture_by_key_[key] = index;
  return index;
}

void Showroom::add_model(const bake::Model& model, const std::vector<std::string>& parts,
                         const float transform[16], const bake::Bake& pack, int axle) {
  const bake::CarInfo& info = car_.car;
  const auto& colours = info.colours;
  const bake::Colour* colour =
      colours.empty() ? nullptr : &colours[std::min(options_.colour, colours.size() - 1)];
  // The livery sheet wherever the car has one (HasLivery's gating is a guess
  // in FORMAT-CARS; the overlay only shows where a livery could go).
  uint32_t livery = UINT32_MAX;
  for (const auto& t : car_.textures)
    if (t.name == "livery_mask") livery = texture_index("livery_mask", car_, true);
  for (const auto& part : model.parts) {
    if (std::find(parts.begin(), parts.end(), part.name) == parts.end()) continue;
    const int32_t best = best_lod(part);
    if (best < 0) continue;
    const auto& pool = best == 0 ? part.lod0_vertices : part.lod_vertices;
    const int32_t vertex_offset = int32_t(vertices_.size());
    vertices_.insert(vertices_.end(), pool.begin(), pool.end());
    bool drawn = false;
    // CARVIEW_SECTIONS=1 lists every section and each drawn one's car-space
    // bounds (debugging which part draws where).
    if (std::getenv("CARVIEW_SECTIONS"))
      for (const auto& s : part.subsections)
        std::printf("section %s/%s lod %d (best %d) %s class %s, %zu indices, slots [%s] [%s], uv0 %g %g x %g %g, "
                    "uv1 %g %g x %g %g\n",
                    model.name.c_str(), part.name.c_str(), int(s.lod), int(best), s.technique.c_str(),
                    bake::material_class_name(s.material), s.indices.size(), s.slots[0].c_str(), s.slots[1].c_str(),
                    s.uv_transform[0], s.uv_transform[1], s.uv_transform[2], s.uv_transform[3], s.uv_transform[4],
                    s.uv_transform[5], s.uv_transform[6], s.uv_transform[7]);
    for (const auto& s : part.subsections) {
      if (s.lod != best || s.indices.empty()) continue;
      if (s.material == MaterialClass::kHidden) continue;
      // `liveryBadge` (not in FORMAT-CARS's technique tables) is the
      // surface a livery's badges go on. Without a livery the game shows
      // nothing there, while its atlas texels are opaque white, which
      // drew a light patch on the Challenger SRT8 392's door and quarter
      // (ours, SPEC_QUESTIONS Q31). Drawn only with --livery.
      if (!options_.livery_overlay && lower(s.technique) == "liverybadge") continue;
      // Car models carry a tyre in their rim ("tire"); tyres are drawn from
      // the tyre model.
      if (s.material == MaterialClass::kTyre && model.role != Role::kTyre) continue;
      Draw d;
      d.first_index = uint32_t(indices_.size());
      d.index_count = uint32_t(s.indices.size());
      d.vertex_offset = vertex_offset;
      bool bad = false;
      std::array<float, 3> sum{};
      for (uint32_t i : s.indices) {
        if (i >= pool.size()) {
          bad = true;
          break;
        }
        const auto p = transform_point(transform, pool[i].position);
        if (!have_bounds_) {
          bounds_min_ = bounds_max_ = p;
          have_bounds_ = true;
        }
        for (int k = 0; k < 3; ++k) {
          bounds_min_[size_t(k)] = std::min(bounds_min_[size_t(k)], p[size_t(k)]);
          bounds_max_[size_t(k)] = std::max(bounds_max_[size_t(k)], p[size_t(k)]);
          sum[size_t(k)] += p[size_t(k)];
        }
      }
      if (bad) {
        notes_.push_back(model.name + "/" + part.name + "/" + s.technique + ": index out of range");
        continue;
      }
      for (int k = 0; k < 3; ++k) d.centre[size_t(k)] = sum[size_t(k)] / float(s.indices.size());
      if (std::getenv("CARVIEW_SECTIONS")) {
        std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (uint32_t i : s.indices) {
          const auto q = transform_point(transform, pool[i].position);
          for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
        }
        std::printf("drawn %s/%s %s: x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", model.name.c_str(), part.name.c_str(),
                    s.technique.c_str(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
      }
      indices_.insert(indices_.end(), s.indices.begin(), s.indices.end());
      d.push.tint[0] = d.push.tint[1] = d.push.tint[2] = d.push.tint[3] = 1.0f;
      std::memcpy(d.push.model, transform, sizeof d.push.model);
      d.wheel = pending_wheel_;
      d.spins = pending_spins_;
      std::memcpy(d.local, pending_local_, sizeof d.local);
      std::memcpy(d.push.uv0, &s.uv_transform[0], 4 * sizeof(float));
      std::memcpy(d.push.uv1, &s.uv_transform[4], 4 * sizeof(float));
      uint32_t flags = 0;
      const std::string technique = lower(s.technique);
      // Classify active emissive lamp channel and re-classify technique BEFORE resolving slot 1
      // so newly classified interior/lamp/carbon/rim/defrost techniques always bind their real textures
      // (both on freshly baked and existing v3 .shiftbake packs).
      d.lamp_channel = drive::classify_lamp_technique(s.technique);
      d.brake_lamp = (d.lamp_channel == drive::LampChannel::kBrake ||
                      d.lamp_channel == drive::LampChannel::kTailBrake2S ||
                      d.lamp_channel == drive::LampChannel::kTail);
      MaterialClass klass = cars::classify(s.technique, s.transparent, model.role);
      if (klass == MaterialClass::kOpaque && d.lamp_channel != drive::LampChannel::kNone) {
        klass = (d.lamp_channel == drive::LampChannel::kGaugeBacklight ||
                 d.lamp_channel == drive::LampChannel::kDigitalGauge ||
                 d.lamp_channel == drive::LampChannel::kNumPlate)
                    ? MaterialClass::kGauge
                    : MaterialClass::kLamp;
      }
      const bool is_defrost = (technique == "defrost_lines");
      // Resolve slot 1 (with fallback for existing v3 .shiftbake packs whose slots[1] was empty).
      std::string slot1 = s.slots[1];
      if (slot1.empty()) {
        if (klass == MaterialClass::kGauge && technique == "gauge_emissive") {
          slot1 = "car:interior_emissive_lod0.xds";
        } else if (klass == MaterialClass::kInterior || klass == MaterialClass::kGauge) {
          slot1 = "car:interior_lod0.xds";
        } else if (klass == MaterialClass::kLamp ||
                   (klass == MaterialClass::kGlass && technique.rfind("detail_glass_", 0) == 0)) {
          slot1 = (s.lod == 0) ? "car:lights_lod0.xds" : "car:lights.xds";
        } else if (klass == MaterialClass::kCarbon) {
          slot1 = "shared:carbonfiber.xds";
        }
      } else if (klass == MaterialClass::kGauge && technique == "gauge_emissive") {
        // Prefer real backlit dial texture when available on gauge_emissive cluster faces.
        if (texture_index("car:interior_emissive_lod0.xds", pack, true) != UINT32_MAX) {
          slot1 = "car:interior_emissive_lod0.xds";
        }
      }
      // Glass techniques never sample the body atlas (FORMAT-CARS, Paint):
      // an opaque glass draw (the windscreen's black_glass band) is a flat
      // dark grey; defrost_lines is rendered as thin translucent copper-amber heater lines.
      const bool flat_glass = (klass != MaterialClass::kGlass && s.slots[0] == "@atlas" &&
                               technique.find("glass") != std::string::npos) ||
                              is_defrost;
      const uint32_t t0 = flat_glass ? UINT32_MAX : texture_index(s.slots[0], pack, s.lod == 0);
      uint32_t t1 = texture_index(slot1, pack, s.lod == 0);
      if (t1 == UINT32_MAX && slot1 == "car:interior_emissive_lod0.xds") {
        t1 = texture_index("car:interior_lod0.xds", pack, s.lod == 0);
      } else if (t1 == UINT32_MAX && slot1 == "car:lights.xds") {
        t1 = texture_index("car:lights_lod0.xds", pack, true);
      }
      if (t0 == UINT32_MAX) flags |= kFlagNoSlot0;
      if (t1 != UINT32_MAX) flags |= kFlagSlot1;
      if (klass == MaterialClass::kPaint) {
        if (colour) {
          d.push.tint[0] = srgb_to_linear(float((colour->rgb >> 16) & 0xFF) / 255.0f);
          d.push.tint[1] = srgb_to_linear(float((colour->rgb >> 8) & 0xFF) / 255.0f);
          d.push.tint[2] = srgb_to_linear(float(colour->rgb & 0xFF) / 255.0f);
          if (colour->metallic) flags |= kFlagMetallic;
          // The stripe constant on the exterior paint techniques of a
          // striped colour row (FORMAT-CARS, Paint).
          d.stripe_able = !(stock_ && stock_->race_livery) && (s.technique == "body" || s.technique == "body_2");
          if (colour->has_stripe && d.stripe_able) {
            flags |= kFlagStripe;
            stripe_[0] = srgb_to_linear(float((colour->stripe_rgb >> 16) & 0xFF) / 255.0f);
            stripe_[1] = srgb_to_linear(float((colour->stripe_rgb >> 8) & 0xFF) / 255.0f);
            stripe_[2] = srgb_to_linear(float(colour->stripe_rgb & 0xFF) / 255.0f);
          }
        }
        if (livery != UINT32_MAX) flags |= kFlagLivery;
        // A factory livery (HasRaceLivery): the texel at every alpha, no
        // paint (FORMAT-CARS, Factory liveries).
        if (stock_ && stock_->race_livery) flags |= kFlagRaceLivery;
      } else if (klass == MaterialClass::kCaliper) {
        const uint32_t rgb = info.caliper_rgb[size_t(axle)];
        d.push.tint[0] = srgb_to_linear(float((rgb >> 16) & 0xFF) / 255.0f);
        d.push.tint[1] = srgb_to_linear(float((rgb >> 8) & 0xFF) / 255.0f);
        d.push.tint[2] = srgb_to_linear(float(rgb & 0xFF) / 255.0f);
      }
      // A grille shows its Shared tile (slot 1) on the tiled UV0; the
      // alpha-tested techniques (FORMAT-CARS: grille1_alpha) cut it out.
      if (t1 != UINT32_MAX && technique.rfind("grille", 0) == 0) {
        flags |= kFlagGrille;
        if (ends_with(technique, "_alpha")) flags |= kFlagCutout;
      }
      if (flat_glass) {
        flags |= kFlagFlat;
        d.push.tint[0] = d.push.tint[1] = d.push.tint[2] = 0.02f;
      }
      // The mirror glass (mirrorLeft, mirrorRight, mirrorMiddle): live rearview
      // mirror reflection in cockpit mode, dynamic environment fallback outside.
      // Store the mirror quad's exact car-space XY bounds in push.uv1 so showroom.frag
      // maps the full 342x256 dedicated mirror band 1:1 across the glass surface.
      if (technique.rfind("mirror", 0) == 0) {
        flags |= kFlagMirror;
        d.push.tint[0] = 0.05f, d.push.tint[1] = 0.055f, d.push.tint[2] = 0.06f;
        float mmin_x = 1e9f, mmax_x = -1e9f, mmin_y = 1e9f, mmax_y = -1e9f;
        for (uint32_t idx : s.indices) {
          if (idx >= pool.size()) continue;
          const auto p = transform_point(transform, pool[idx].position);
          mmin_x = std::min(mmin_x, p[0]);
          mmax_x = std::max(mmax_x, p[0]);
          mmin_y = std::min(mmin_y, p[1]);
          mmax_y = std::max(mmax_y, p[1]);
        }
        const float sx = std::max(mmax_x - mmin_x, 1e-3f);
        const float sy = std::max(mmax_y - mmin_y, 1e-3f);
        d.push.uv1[0] = mmin_x;
        d.push.uv1[1] = 1.0f / sx;
        d.push.uv1[2] = mmin_y;
        d.push.uv1[3] = 1.0f / sy;
      }
      {
        const std::string pname = lower(part.name);
        if (pname == "steering_wheel") d.cockpit_role = 0;
        else if (pname == "speed_min") d.cockpit_role = 1;
        else if (pname == "tach_min") d.cockpit_role = 2;
        else if (pname == "fuel_min") d.cockpit_role = 3;
        else if (pname == "boost_min") d.cockpit_role = 4;
        else if (pname == "power_min") d.cockpit_role = 5;
      }
      // Lamp covers (detail_glass_red, _amber, _clear): passive coloured glass
      // (CAR_LAMPS.md §3.2 — covers NEVER emit). Rendered in two FH1 passes (multiply + add).
      if (klass == MaterialClass::kGlass && technique.rfind("detail_glass_", 0) == 0) {
        const std::string hue = technique.substr(13);
        float c[3] = {0.55f, 0.58f, 0.6f};
        if (hue.rfind("red", 0) == 0) c[0] = 0.22f, c[1] = 0.006f, c[2] = 0.005f;
        else if (hue.rfind("amber", 0) == 0) c[0] = 0.6f, c[1] = 0.22f, c[2] = 0.01f;
        flags |= kFlagCover;
        d.is_cover = true;
        std::memcpy(d.push.tint, c, sizeof c);
      }
      // Resolve per-draw normal map (slot 3, linear srgb=false) and damage atlas (slot 4).
      auto try_tex = [&](std::initializer_list<const char*> candidates) -> uint32_t {
        for (const char* c : candidates) {
          const uint32_t idx = texture_index(c, pack, s.lod == 0);
          if (idx != UINT32_MAX) return idx;
        }
        return UINT32_MAX;
      };
      uint32_t t_nrm = UINT32_MAX;
      bool nrm_uv1 = false;
      if (klass == MaterialClass::kCarbon) {
        t_nrm = try_tex({"shared:carbonfibernormal.xds"});
        nrm_uv1 = true;
      } else if (klass == MaterialClass::kTyre) {
        if (technique == "sidewall") t_nrm = try_tex({"tyre:sidewall_nrm"});
        else if (technique == "tread") t_nrm = try_tex({"tyre:tread_nrm"});
      } else if (klass == MaterialClass::kInterior) {
        nrm_uv1 = true;
        if (technique == "bump_leather2" || technique == "leather2") {
          t_nrm = try_tex({"car:leather2_nrm.xds", "shared:leather2_nrm.xds", "shared:leather_nrm.xds"});
        } else if (technique == "bump_leather3" || technique == "leather3") {
          t_nrm = try_tex({"car:leather3_nrm.xds", "shared:leather3_nrm.xds", "shared:leather_nrm.xds"});
        } else if (technique == "bump_leather" || technique == "leather") {
          t_nrm = try_tex({"car:leather_nrm.xds", "shared:leather_nrm.xds"});
        } else if (technique == "bump_plastic2" || technique == "plastic2") {
          t_nrm = try_tex({"car:plastic2_nrm.xds", "shared:plastic2_nrm.xds", "shared:plastic_nrm.xds"});
        } else if (technique == "bump_plastic3" || technique == "plastic3") {
          t_nrm = try_tex({"car:plastic3_nrm.xds", "shared:plastic3_nrm.xds", "shared:plastic_nrm.xds"});
        } else if (technique == "bump_plastic" || technique == "plastic") {
          t_nrm = try_tex({"car:plastic_nrm.xds", "shared:plastic_nrm.xds"});
        } else if (technique == "stitching" || technique == "bump_stitching") {
          t_nrm = try_tex({"car:stitches_nrm.xds", "shared:stitches_nrm.xds"});
        } else if (technique == "cloth" || technique == "cloth2") {
          t_nrm = try_tex({"shared:cloth_nrm_s_lod0.xds"});
        } else if (technique == "headliner" || technique == "headliner2") {
          t_nrm = try_tex({"shared:headliner_nrm_s_lod0.xds"});
        } else if (technique == "mottled" || technique == "mottled2") {
          t_nrm = try_tex({"shared:mottled_nrm_s_lod0.xds"});
        }
      }
      if (t_nrm != UINT32_MAX) {
        flags |= kFlagNormalMap;
        if (nrm_uv1) flags |= kFlagNormalUv1;
      }
      uint32_t t_dmg = UINT32_MAX;
      if ((klass == MaterialClass::kPaint || klass == MaterialClass::kOpaque) && s.slots[0] == "@atlas") {
        t_dmg = try_tex({"car:damage_lod0.xds", "car:damage.xds"});
      } else if (klass == MaterialClass::kLamp) {
        t_dmg = try_tex({"car:damagelights_lod0.xds", "car:damagelights.xds"});
      }
      if (t_dmg != UINT32_MAX) flags |= kFlagDamageMap;
      // Alpha-tested emissive gauge overlays (z_gauge_emissive_alpha, gauge_emissive_alpha):
      // discard transparent surround so no solid rectangle blocks the gauge dials.
      if (klass == MaterialClass::kGauge &&
          (ends_with(technique, "_alpha") || technique.find("alpha") != std::string::npos)) {
        flags |= kFlagCutout;
      }
      d.push.tint[3] = 1;
      // Sub-material variant ID encoded in params[2] for showroom.frag:
      // 1=outer_rim (chrome lip), 2=inner_rim/wheel_black (dark barrel), 3=rotor/brake/drum,
      // 4=hub, 5=cockpit metal/metal2, 6=defrost_lines, 10=mirrorLeft, 11=mirrorMiddle, 12=mirrorRight,
      // 15=cockpit gauge needle, 16=cockpit digital MFD gauge.
      float submat_id = 0.0f;
      if (is_defrost) {
        submat_id = 6.0f;
      } else if (technique.rfind("mirror", 0) == 0) {
        if (technique == "mirrorleft") submat_id = 10.0f;
        else if (technique == "mirrorright") submat_id = 12.0f;
        else submat_id = 11.0f;
      } else if (d.cockpit_role >= 1 || technique == "gauge_needle" || technique == "cockpit_gauge_needle") {
        submat_id = 15.0f;
      } else if (d.lamp_channel == drive::LampChannel::kDigitalGauge) {
        submat_id = 16.0f;
        // Pack the MFD screen's local car-space XY bounds into push.uv1 so showroom.frag
        // renders right-side-up active backlit telemetry across any car's digital cluster.
        float dmin_x = 1e9f, dmax_x = -1e9f, dmin_y = 1e9f, dmax_y = -1e9f;
        for (uint32_t idx : s.indices) {
          if (idx >= pool.size()) continue;
          const auto p = transform_point(transform, pool[idx].position);
          dmin_x = std::min(dmin_x, p[0]);
          dmax_x = std::max(dmax_x, p[0]);
          dmin_y = std::min(dmin_y, p[1]);
          dmax_y = std::max(dmax_y, p[1]);
        }
        const float dx = std::max(dmax_x - dmin_x, 1e-3f);
        const float dy = std::max(dmax_y - dmin_y, 1e-3f);
        d.push.uv1[0] = dmin_x;
        d.push.uv1[1] = 1.0f / dx;
        d.push.uv1[2] = dmin_y;
        d.push.uv1[3] = 1.0f / dy;
      } else if (klass == MaterialClass::kGauge && slot1 == "car:interior_emissive_lod0.xds" && t1 != UINT32_MAX) {
        submat_id = 17.0f;
      } else if (klass == MaterialClass::kRim) {
        if (technique == "outer_rim") submat_id = 1.0f;
        else if (technique == "inner_rim" || technique == "wheel_black") submat_id = 2.0f;
        else if (technique == "rotor" || technique == "brake" || technique == "drum") submat_id = 3.0f;
        else if (technique == "hub") submat_id = 4.0f;
      } else if (klass == MaterialClass::kInterior) {
        std::string base_name = technique;
        while (!base_name.empty() && std::isdigit(static_cast<unsigned char>(base_name.back()))) base_name.pop_back();
        if (base_name == "metal") submat_id = 5.0f;
        // Preserve authentic textured interior contrast without crushing dark leather/plastics to pitch black.
        const float k = base_name == "mottled" ? 0.80f :
                        base_name == "bump_plastic" ? 0.78f :
                        base_name == "metal" ? 1.0f : 0.90f;
        d.push.tint[0] = d.push.tint[1] = d.push.tint[2] = k;
      }
      d.push.params[0] = float(uint32_t(klass));
      d.push.params[1] = float(flags);
      d.push.params[2] = submat_id;
      d.textures = {t0 == UINT32_MAX ? 0u : t0, t1 == UINT32_MAX ? 0u : t1,
                    livery == UINT32_MAX ? 1u : livery, t_nrm, t_dmg};
      d.glass = (klass == MaterialClass::kGlass);
      d.part_name = lower(part.name);
      draws_.push_back(d);
      drawn = true;
    }
    if (drawn) drawn_parts_.push_back(model.name + "/" + part.name);
  }
}

void Showroom::set_braking(float glow) {
  rotor_glow_ = std::clamp(glow, 0.0f, 1.0f);
  if (!lamp_state_active_) {
    lamp_vectors_.dynamic_lights_amount[0] = rotor_glow_;
  }
  for (Draw& d : draws_)
    if (d.brake_lamp) d.push.params[2] = glow;
}

void Showroom::set_lamp_state(const drive::CarLampState& state) {
  lamp_vectors_ = state.vectors();
  lamp_state_active_ = true;
}

void Showroom::set_lamp_vectors(const drive::LampShaderVectors& vectors) {
  lamp_vectors_ = vectors;
  lamp_state_active_ = true;
}

void Showroom::set_cockpit_wheel_offsets(std::array<float, 3> pos_offset_m, float rot_offset_deg) {
  cockpit_wheel_pos_offset_ = pos_offset_m;
  cockpit_wheel_rot_offset_rad_ = rot_offset_deg * (3.14159265358979323846f / 180.0f);
}

void Showroom::set_cockpit_anim(const drive::CockpitAnimState& anim, bool cockpit_camera) {
  mirror_enabled_ = cockpit_camera;
  cockpit_camera_active_ = cockpit_camera;
  cockpit_mfd_[0] = std::clamp(anim.speed_norm, 0.0f, 1.0f);
  cockpit_mfd_[1] = std::clamp(anim.tach_norm, 0.0f, 1.0f);
  cockpit_mfd_[2] = std::clamp(anim.fuel_norm, 0.0f, 1.0f);
  cockpit_mfd_[3] = std::clamp(std::max(anim.boost_norm, anim.power_norm), 0.0f, 1.0f);
  for (int k = 0; k < 4; ++k) frame_.cockpit_mfd[k] = cockpit_mfd_[k];
  if (cockpit_camera && dyn_env_ready_ && !mirror_tex_.view) ensure_mirror_targets();
  driver_wheel_rad_ = anim.wheel_rad;
  update_driver_mesh(anim.wheel_rad);
  auto make_rot = [](const std::array<float, 3>& pivot, const std::array<float, 3>& axis, float angle, float out[16]) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    const float r00 = t * x * x + c,     r01 = t * x * y - s * z, r02 = t * x * z + s * y;
    const float r10 = t * x * y + s * z, r11 = t * y * y + c,     r12 = t * y * z - s * x;
    const float r20 = t * x * z - s * y, r21 = t * y * z + s * x, r22 = t * z * z + c;
    const float px = pivot[0], py = pivot[1], pz = pivot[2];
    out[0] = r00; out[4] = r01; out[8]  = r02; out[12] = px - (r00 * px + r01 * py + r02 * pz);
    out[1] = r10; out[5] = r11; out[9]  = r12; out[13] = py - (r10 * px + r11 * py + r12 * pz);
    out[2] = r20; out[6] = r21; out[10] = r22; out[14] = pz - (r20 * px + r21 * py + r22 * pz);
    out[3] = 0;   out[7] = 0;   out[11] = 0;   out[15] = 1;
  };
  const float norms[5] = {anim.speed_norm, anim.tach_norm, anim.fuel_norm, anim.boost_norm, anim.power_norm};
  for (Draw& d : draws_) {
    if (d.cockpit_role < 0 || d.cockpit_role >= 6) continue;
    const auto& rig = cockpit_rigs_[d.cockpit_role];
    if (!rig.valid) continue;
    float angle = 0.0f;
    if (d.cockpit_role == 0) {
      angle = anim.wheel_rad;
    } else {
      const float n = std::clamp(norms[d.cockpit_role - 1], 0.0f, 1.0f);
      angle = n <= 0.5f ? (n * 2.0f) * rig.ang_mid : rig.ang_mid + ((n - 0.5f) * 2.0f) * (rig.ang_max - rig.ang_mid);
    }
    make_rot(rig.pivot, rig.axis, angle, d.push.model);
    if (d.cockpit_role == 0) {
      // Apply CameraOverrides CockpitWheelRotationOffset (tilt about X at pivot)
      // and CockpitWheelPositionOffset{X,Y,Z} (body frame -> model frame mirrors Z).
      if (std::fabs(cockpit_wheel_rot_offset_rad_) > 1e-5f) {
        float tilt[16], combined[16];
        make_rot(rig.pivot, {1.0f, 0.0f, 0.0f}, cockpit_wheel_rot_offset_rad_, tilt);
        multiply(tilt, d.push.model, combined);
        std::memcpy(d.push.model, combined, sizeof(combined));
      }
      d.push.model[12] += cockpit_wheel_pos_offset_[0];
      d.push.model[13] += cockpit_wheel_pos_offset_[1];
      d.push.model[14] -= cockpit_wheel_pos_offset_[2];
    }
  }
}

void Showroom::set_wheel_poses(const WheelPose poses[4]) {
  for (int i = 0; i < 4; ++i) wheel_poses_[i] = poses[i];
  have_wheel_poses_ = true;
  for (Draw& d : draws_) {
    if (d.wheel < 0) continue;
    const WheelPose& p = poses[d.wheel];
    // Body frame: centre, steer about y, spin about x (outside the right
    // wheels' half turn, so every wheel rolls forward), then the draw's own
    // local matrix; then the inverse pose to the model frame.
    const float cs = std::cos(p.steer), ss = std::sin(p.steer);
    const float steer[16] = {cs, 0, -ss, 0, 0, 1, 0, 0, ss, 0, cs, 0, p.centre[0], p.centre[1], p.centre[2], 1};
    float spin[16];
    identity(spin);
    if (d.spins) {
      const float cr = std::cos(p.spin), sr = std::sin(p.spin);
      spin[5] = cr;
      spin[6] = sr;
      spin[9] = -sr;
      spin[10] = cr;
    }
    float a[16], b[16];
    multiply(steer, spin, a);
    multiply(a, d.local, b);
    multiply(body_to_model_, b, d.push.model);
    if (d.spins) {
      const int klass = int(d.push.params[0] + 0.5f);
      if (klass == 7 || klass == 9) {  // kRim or kChrome: blur_rim / chrome_blur_rim blend
        d.push.params[3] = world::wheel_blur_factor(p.omega);
      }
    }
  }
}

// The levels of `texture` that form a mip chain with valid sizes, and
// their bytes.
static uint32_t texture_chain(const bake::Texture& texture, VkDeviceSize& total) {
  total = 0;
  if (texture.levels.empty()) return 0;
  const uint32_t w0 = texture.levels[0].width, h0 = texture.levels[0].height;
  uint32_t levels = 0;
  for (const auto& level : texture.levels) {
    const uint32_t w = std::max(1u, w0 >> levels), h = std::max(1u, h0 >> levels);
    if (level.width != w || level.height != h || level.data.size() != level_bytes(texture.format, w, h)) break;
    total += level.data.size();
    ++levels;
  }
  return levels;
}

bool Showroom::create_texture_image(const bake::Texture& texture, bool srgb, uint32_t levels, GpuTexture& out,
                                    std::string& error, uint32_t skip) {
  if (skip >= levels) skip = levels ? levels - 1 : 0;
  const uint32_t kept = levels - skip;
  const VkFormat format = vk_format(texture.format, srgb);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {texture.levels[skip].width, texture.levels[skip].height, 1};
  info.mipLevels = kept;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo alloc_info{};
  VK_TRY(vmaCreateImage(r_.allocator_, &info, &alloc, &out.image, &out.allocation, &alloc_info),
         "vmaCreateImage");
  out.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.components = {vk_swizzle(texture.swizzle[0]), vk_swizzle(texture.swizzle[1]),
                     vk_swizzle(texture.swizzle[2]), vk_swizzle(texture.swizzle[3])};
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kept, 0, 1};
  if (vkCreateImageView(r_.device_, &view, nullptr, &out.view) != VK_SUCCESS) {
    r_.destroy_image(out);
    error = "vkCreateImageView failed";
    return false;
  }
  return true;
}

// Copies levels `[skip .. levels)` packed from `offset` in `staging` into `image`
// and leaves it ready to sample.
static void record_texture_copy(VkCommandBuffer cmd, VkBuffer staging, VkDeviceSize offset, const bake::Texture& texture,
                                uint32_t levels, VkImage image, uint32_t skip = 0) {
  if (skip >= levels) skip = levels ? levels - 1 : 0;
  const uint32_t kept = levels - skip;
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t i = skip; i < levels; ++i) {
    const auto& level = texture.levels[i];
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - skip, 0, 1};
    copy.imageExtent = {level.width, level.height, 1};
    copies.push_back(copy);
    offset += level.data.size();
  }
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kept, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
  vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                         copies.data());
  b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier2(cmd, &dep);
}

bool Showroom::upload_texture(const bake::Texture* texture, bool srgb, GpuTexture& out,
                              std::string& error) {
  // Built-ins: 1x1 white or black.
  bake::Texture builtin;
  if (!texture) {
    builtin.format = bake::TextureFormat::kRgba8;
    builtin.levels.push_back({1, 1, {255, 255, 255, 255}});
    texture = &builtin;
  }
  // A mod's replacement texture (texture_mods.h), by content hash or name.
  bake::Texture replaced;
  if (texture != &builtin && cars::texture_mods::any()) {
    std::string why;
    if (cars::texture_mods::substitute(*texture, replaced, &why)) texture = &replaced;
    else if (!why.empty()) notes_.push_back("replacement texture " + why);
  }
  VkDeviceSize total = 0;
  const uint32_t levels = texture_chain(*texture, total);
  if (!levels) {
    error = "texture " + texture->name + ": level 0 has the wrong size";
    return false;
  }
  const uint32_t skip = modern::texture_levels_to_skip(texture->levels[0].width, texture->levels[0].height, levels,
                                                       texture_cap_);
  VkDeviceSize kept_bytes = 0;
  for (uint32_t i = skip; i < levels; ++i) kept_bytes += texture->levels[i].data.size();
  if (!create_texture_image(*texture, srgb, levels, out, error, skip)) return false;
  Renderer::Buffer staging;
  if (!r_.create_buffer(kept_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        staging)) {
    r_.destroy_image(out);
    error = "texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  if (vmaMapMemory(r_.allocator_, staging.allocation, reinterpret_cast<void**>(&mapped)) != VK_SUCCESS) {
    r_.destroy_buffer(staging);
    r_.destroy_image(out);
    error = "texture staging map";
    return false;
  }
  VkDeviceSize offset = 0;
  for (uint32_t i = skip; i < levels; ++i) {
    std::memcpy(mapped + offset, texture->levels[i].data.data(), texture->levels[i].data.size());
    offset += texture->levels[i].data.size();
  }
  vmaUnmapMemory(r_.allocator_, staging.allocation);
  const bool ok = r_.submit_once(
      [&](VkCommandBuffer cmd) { record_texture_copy(cmd, staging.buffer, 0, *texture, levels, out.image, skip); });
  r_.destroy_buffer(staging);
  if (!ok) {
    error = "texture upload";
    return false;
  }
  return true;
}

bool Showroom::upload_textures_batch(const std::vector<TextureUploadItem>& items, std::string& error) {
  if (items.empty()) return true;
  bake::Texture builtin_white;
  builtin_white.format = bake::TextureFormat::kRgba8;
  builtin_white.levels.push_back({1, 1, {255, 255, 255, 255}});

  struct PreparedTex {
    bake::Texture owned_copy;
    const bake::Texture* tex = nullptr;
    GpuTexture* dst = nullptr;
    uint32_t levels = 0;
    uint32_t skip = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize bytes = 0;
  };
  std::vector<PreparedTex> prepared;
  prepared.reserve(items.size());
  VkDeviceSize total_bytes = 0;
  constexpr VkDeviceSize kAlign = 16;

  auto cleanup_all = [&]() {
    for (auto& p : prepared) {
      if (p.dst) r_.destroy_image(*p.dst);
    }
  };

  for (const auto& item : items) {
    if (!item.dst) continue;
    PreparedTex p;
    p.dst = item.dst;
    const bake::Texture* src = item.texture ? item.texture : &builtin_white;
    bool srgb = item.srgb;
    if (src != &builtin_white && cars::texture_mods::any()) {
      std::string why;
      if (cars::texture_mods::substitute(*src, p.owned_copy, &why)) src = &p.owned_copy;
      else if (!why.empty()) notes_.push_back("replacement texture " + why);
    }
    VkDeviceSize total = 0;
    uint32_t levels = texture_chain(*src, total);
    uint32_t skip = levels ? modern::texture_levels_to_skip(src->levels[0].width, src->levels[0].height, levels,
                                                            texture_cap_)
                           : 0;
    std::string local_err;
    if (!levels || !create_texture_image(*src, srgb, levels, *p.dst, local_err, skip)) {
      if (!item.fallback_white) {
        error = levels ? local_err : ("texture " + src->name + ": level 0 has the wrong size");
        cleanup_all();
        return false;
      }
      notes_.push_back(levels ? local_err : ("texture " + src->name + ": level 0 has the wrong size"));
      r_.destroy_image(*p.dst);
      src = &builtin_white;
      srgb = true;
      levels = 1;
      skip = 0;
      if (!create_texture_image(*src, srgb, levels, *p.dst, error, 0)) {
        cleanup_all();
        return false;
      }
    }
    VkDeviceSize kept_bytes = 0;
    for (uint32_t i = skip; i < levels; ++i) kept_bytes += src->levels[i].data.size();
    total_bytes = (total_bytes + kAlign - 1) & ~(kAlign - 1);
    p.tex = src;
    p.levels = levels;
    p.skip = skip;
    p.offset = total_bytes;
    p.bytes = kept_bytes;
    total_bytes += kept_bytes;
    prepared.push_back(std::move(p));
  }
  if (prepared.empty() || total_bytes == 0) return true;

  Renderer::Buffer staging;
  if (!r_.create_buffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
    cleanup_all();
    error = "batched texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  if (vmaMapMemory(r_.allocator_, staging.allocation, reinterpret_cast<void**>(&mapped)) != VK_SUCCESS) {
    r_.destroy_buffer(staging);
    cleanup_all();
    error = "batched texture staging map";
    return false;
  }
  for (const auto& p : prepared) {
    VkDeviceSize off = p.offset;
    for (uint32_t i = p.skip; i < p.levels; ++i) {
      std::memcpy(mapped + off, p.tex->levels[i].data.data(), p.tex->levels[i].data.size());
      off += p.tex->levels[i].data.size();
    }
  }
  vmaUnmapMemory(r_.allocator_, staging.allocation);

  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    std::vector<VkImageMemoryBarrier2> pre_barriers;
    std::vector<VkImageMemoryBarrier2> post_barriers;
    pre_barriers.reserve(prepared.size());
    post_barriers.reserve(prepared.size());
    for (const auto& p : prepared) {
      const uint32_t kept = p.levels - p.skip;
      VkImageMemoryBarrier2 pre{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      pre.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      pre.srcQueueFamilyIndex = pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre.image = p.dst->image;
      pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kept, 0, 1};
      pre_barriers.push_back(pre);

      VkImageMemoryBarrier2 post = pre;
      post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      post.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      post_barriers.push_back(post);
    }
    VkDependencyInfo pre_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    pre_dep.imageMemoryBarrierCount = uint32_t(pre_barriers.size());
    pre_dep.pImageMemoryBarriers = pre_barriers.data();
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    std::vector<VkBufferImageCopy> copies;
    for (const auto& p : prepared) {
      copies.clear();
      VkDeviceSize off = p.offset;
      for (uint32_t i = p.skip; i < p.levels; ++i) {
        const auto& lvl = p.tex->levels[i];
        VkBufferImageCopy c{};
        c.bufferOffset = off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - p.skip, 0, 1};
        c.imageExtent = {lvl.width, lvl.height, 1};
        copies.push_back(c);
        off += lvl.data.size();
      }
      vkCmdCopyBufferToImage(cmd, staging.buffer, p.dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(copies.size()), copies.data());
    }

    VkDependencyInfo post_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    post_dep.imageMemoryBarrierCount = uint32_t(post_barriers.size());
    post_dep.pImageMemoryBarriers = post_barriers.data();
    vkCmdPipelineBarrier2(cmd, &post_dep);
  });
  r_.destroy_buffer(staging);
  if (!ok) {
    cleanup_all();
    error = "batched texture upload";
    return false;
  }
  return true;
}

bool Showroom::upload_environment(std::string& error) {
  uint32_t kSize = 64, kLevels = 7;
  std::vector<std::vector<std::vector<std::array<float, 3>>>> faces;
  bool loaded_xpr = false;
  if (!guest_ && !env_xpr_path_.empty() && std::filesystem::exists(env_xpr_path_)) {
    if (FILE* f = std::fopen(env_xpr_path_.string().c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      const long sz = std::ftell(f);
      if (sz > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(sz));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
          fh1::XprCubemap xpr;
          std::string xerr;
          if (fh1::decode_xpr_cubemap(raw, xpr, &xerr) && xpr.size > 0 && (xpr.size & (xpr.size - 1)) == 0) {
            kSize = xpr.size;
            kLevels = 1;
            for (uint32_t s = kSize; s > 1; s >>= 1) ++kLevels;
            faces.resize(kLevels);
            faces[0].resize(6);
            for (uint32_t face = 0; face < 6; ++face) faces[0][face] = std::move(xpr.faces[face]);
            loaded_xpr = true;
          }
        }
      }
      std::fclose(f);
    }
  }
  if (!loaded_xpr) {
    faces.resize(kLevels);
    // Level 0 from the studio function, then 2x2 box filters per face.
    for (uint32_t face = 0; face < 6; ++face) {
      std::vector<std::array<float, 3>> texels(size_t(kSize) * kSize);
      for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x) {
          const float u = (float(x) + 0.5f) / kSize * 2 - 1, v = (float(y) + 0.5f) / kSize * 2 - 1;
          // Vulkan cube face orientation (+X, -X, +Y, -Y, +Z, -Z).
          float d[3];
          switch (face) {
            case 0: d[0] = 1; d[1] = -v; d[2] = -u; break;
            case 1: d[0] = -1; d[1] = -v; d[2] = u; break;
            case 2: d[0] = u; d[1] = 1; d[2] = v; break;
            case 3: d[0] = u; d[1] = -1; d[2] = -v; break;
            case 4: d[0] = u; d[1] = -v; d[2] = 1; break;
            default: d[0] = -u; d[1] = -v; d[2] = -1; break;
          }
          texels[size_t(y) * kSize + x] = studio(d[0], d[1], d[2]);
        }
      faces[0].push_back(std::move(texels));
    }
  }
  for (uint32_t level = 1; level < kLevels; ++level) {
    const uint32_t s = kSize >> level, p = s * 2;
    for (uint32_t face = 0; face < 6; ++face) {
      const auto& prev = faces[level - 1][face];
      std::vector<std::array<float, 3>> texels(size_t(s) * s);
      for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x)
          for (int c = 0; c < 3; ++c)
            texels[size_t(y) * s + x][size_t(c)] =
                0.25f * (prev[(2 * y) * p + 2 * x][size_t(c)] + prev[(2 * y) * p + 2 * x + 1][size_t(c)] +
                         prev[(2 * y + 1) * p + 2 * x][size_t(c)] +
                         prev[(2 * y + 1) * p + 2 * x + 1][size_t(c)]);
      faces[level].push_back(std::move(texels));
    }
  }
  std::vector<uint16_t> halfs;
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t level = 0; level < kLevels; ++level)
    for (uint32_t face = 0; face < 6; ++face) {
      VkBufferImageCopy copy{};
      copy.bufferOffset = halfs.size() * 2;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
      copy.imageExtent = {kSize >> level, kSize >> level, 1};
      copies.push_back(copy);
      for (const auto& t : faces[level][face]) {
        halfs.push_back(to_half(t[0]));
        halfs.push_back(to_half(t[1]));
        halfs.push_back(to_half(t[2]));
        halfs.push_back(to_half(1));
      }
    }
  const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {kSize, kSize, 1};
  info.mipLevels = kLevels;
  info.arrayLayers = 6;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo alloc_info{};
  VK_TRY(vmaCreateImage(r_.allocator_, &info, &alloc, &environment_.image, &environment_.allocation, &alloc_info),
         "vmaCreateImage (cube)");
  environment_.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = environment_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, 6};
  if (vkCreateImageView(r_.device_, &view, nullptr, &environment_.view) != VK_SUCCESS) {
    r_.destroy_image(environment_);
    error = "vkCreateImageView (cube)";
    return false;
  }
  Renderer::Buffer staging;
  const VkDeviceSize bytes = halfs.size() * 2;
  if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        staging)) {
    error = "environment staging buffer";
    return false;
  }
  void* mapped = nullptr;
  if (vmaMapMemory(r_.allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
    r_.destroy_buffer(staging);
    error = "environment staging map";
    return false;
  }
  std::memcpy(mapped, halfs.data(), size_t(bytes));
  vmaUnmapMemory(r_.allocator_, staging.allocation);
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = environment_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, 6};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdCopyBufferToImage(cmd, staging.buffer, environment_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           uint32_t(copies.size()), copies.data());
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
  });
  r_.destroy_buffer(staging);
  if (!ok) error = "environment upload";
  return ok;
}

bool Showroom::create_pipelines(std::string& error) {
  // Set 0: frame UBO, dynamic/active environment cube, sun shadow map (PCF compare), headlight cookie,
  // headlights UBO, mirror map, car instances SSBO (6), static studio cube (7, staticcarcubemap.xpr),
  // shared dirt pattern (8), dirt mask (9), cracked glass (10), and raw D32 shadow depth (11, PCSS blocker search).
  const VkDescriptorSetLayoutBinding frame_bindings[12] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 12;
  set.pBindings = frame_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &frame_layout_), "frame set layout");
  VkDescriptorSetLayoutBinding material_bindings[5];
  for (uint32_t i = 0; i < 5; ++i)
    material_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr};
  set.bindingCount = 5;
  set.pBindings = material_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &material_layout_),
         "material set layout");
  VkDescriptorSetLayoutBinding world_material_bindings[8];
  for (uint32_t i = 0; i < 8; ++i)
    world_material_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                  nullptr};
  set.bindingCount = 8;
  set.pBindings = world_material_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &world_material_layout_),
         "world material set layout");
  const VkDescriptorSetLayout layouts[2] = {frame_layout_, material_layout_};
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 2;
  layout.pSetLayouts = layouts;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &layout, nullptr, &pipeline_layout_), "pipeline layout");
  const VkDescriptorSetLayout world_layouts[2] = {frame_layout_, world_material_layout_};
  layout.pSetLayouts = world_layouts;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &layout, nullptr, &world_pipeline_layout_), "world pipeline layout");

  // A mod's override (shader_mods.h) replaces a built-in shader of the same name.
  std::vector<std::vector<uint32_t>> override_code;
  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module, const char* name = nullptr) {
    if (name)
      if (auto o = cars::shader_mods::registry().find(name)) {
        override_code.push_back(std::move(*o));
        code = override_code.back().data();
        bytes = override_code.back().size() * sizeof(uint32_t);
        notes_.push_back(std::string("shader override: ") + name);
      }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(r_.device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, mask = VK_NULL_HANDLE;
  VK_TRY(make_module(kShowroom_vert_spv, sizeof(kShowroom_vert_spv), vert, "showroom.vert"), "showroom.vert");
  VK_TRY(make_module(kShowroom_frag_spv, sizeof(kShowroom_frag_spv), frag, "showroom.frag"), "showroom.frag");
  VK_TRY(make_module(kMask_frag_spv, sizeof(kMask_frag_spv), mask, "mask.frag"), "mask.frag");

  static_assert(sizeof(bake::Vertex) == 32);
  VkVertexInputBindingDescription binding{0, sizeof(bake::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription attributes[5] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bake::Vertex, position)},
      {1, 0, VK_FORMAT_R16G16B16A16_SNORM, offsetof(bake::Vertex, frame)},
      {2, 0, VK_FORMAT_R16G16_UNORM, offsetof(bake::Vertex, uv0)},
      {3, 0, VK_FORMAT_R16G16_UNORM, offsetof(bake::Vertex, uv1)},
      {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(bake::Vertex, lighting)}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 5;
  vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // Both sides: the shader faces the normal towards the eye.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  // Under the drive's post chain the scene shaders write linear HDR
  // (constant 0 of showroom.frag and world.frag).
  // Constant 1 (world.vert): reversed depth, for the sky's depth.
  const VkBool32 spec_values[2] = {r_.post() ? VK_TRUE : VK_FALSE, r_.reverse_z() ? VK_TRUE : VK_FALSE};
  const VkSpecializationMapEntry spec_entries[2] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)}};
  const VkSpecializationInfo spec{2, spec_entries, sizeof spec_values, spec_values};
  // The world's pipelines take a depth bias per draw (decals and road
  // paint over the surface they lie on; the rest 0).
  VkPipelineRasterizationStateCreateInfo raster_bias = raster;
  raster_bias.depthBiasEnable = VK_TRUE;
  const VkDynamicState bias_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dynamic_bias{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_bias.dynamicStateCount = 3;
  dynamic_bias.pDynamicStates = bias_states;
  const VkDynamicState vrs_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic_vrs{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_vrs.dynamicStateCount = 3;
  dynamic_vrs.pDynamicStates = vrs_states;
  const VkDynamicState vrs_bias_states[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic_bias_vrs{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_bias_vrs.dynamicStateCount = 4;
  dynamic_bias_vrs.pDynamicStates = vrs_bias_states;
  auto build = [&](VkShaderModule fragment, VkFormat color_format, VkSampleCountFlagBits samples,
                   bool depth_attachment, bool depth_write, int blend_mode, VkPipeline& out,
                   VkShaderModule vertex = VK_NULL_HANDLE, const VkPipelineVertexInputStateCreateInfo* input = nullptr,
                   bool coverage = false, VkPipelineLayout pipe_layout = VK_NULL_HANDLE,
                   VkCullModeFlags cull_mode = VK_CULL_MODE_NONE, bool depth_equal = false, bool color_out = true) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vertex ? vertex : vert, "main", vertex ? &spec : nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         fragment, "main", fragment == mask ? nullptr : &spec}};
    VkPipelineRasterizationStateCreateInfo rstate = vertex ? raster_bias : raster;
    rstate.cullMode = cull_mode;
    if (vertex) rstate.frontFace = VK_FRONT_FACE_CLOCKWISE;  // world_transform_ flips det = -1
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    multisample.alphaToCoverageEnable = coverage && samples != VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth_attachment;
    depth.depthWriteEnable = depth_write;
    depth.depthCompareOp = depth_equal ? VK_COMPARE_OP_EQUAL
                                       : (r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL);
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = (coverage && r_.post())
                                    ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)
                                    : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                       VK_COLOR_COMPONENT_A_BIT);
    if (blend_mode == 1 || blend_mode == 4) {
      // Premultiplied colour over what is behind; blend_mode == 4 (water) also writes surface gloss alpha for SSR.
      attachment.blendEnable = VK_TRUE;
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      attachment.colorBlendOp = VK_BLEND_OP_ADD;
      attachment.srcAlphaBlendFactor = (blend_mode == 4) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
      attachment.dstAlphaBlendFactor = (blend_mode == 4) ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
      attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend_mode == 2) {
      // Multiply blend (dst = src * dst): FH1 detail_glass_* Pass 1 & under-car contact shadow.
      attachment.blendEnable = VK_TRUE;
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
      attachment.colorBlendOp = VK_BLEND_OP_ADD;
      attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend_mode == 3) {
      // Additive blend (dst = src + dst): FH1 detail_glass_* Pass 2 reflection + sun glint.
      attachment.blendEnable = VK_TRUE;
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.colorBlendOp = VK_BLEND_OP_ADD;
      attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = color_out ? 1u : 0u;
    blend.pAttachments = color_out ? &attachment : nullptr;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = color_out ? 1u : 0u;
    rendering.pColorAttachmentFormats = color_out ? &color_format : nullptr;
    rendering.depthAttachmentFormat = depth_attachment ? r_.depth_format_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = fragment ? 2u : 1u;
    info.pStages = stages;
    info.pVertexInputState = input ? input : &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rstate;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    const bool use_vrs = r_.vrs_supported() && color_out && (blend_mode == 0) && samples == r_.samples_ &&
                         (&out != &env_world_opaque_) && (&out != &env_world_coverage_) && (&out != &env_car_opaque_);
    info.pDynamicState = vertex ? (use_vrs ? &dynamic_bias_vrs : &dynamic_bias)
                                : (use_vrs ? &dynamic_vrs : &dynamic);
    info.layout = pipe_layout ? pipe_layout : pipeline_layout_;
    return vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &info, nullptr, &out);
  };
  const VkCullModeFlags cull_back = r_.backface_cull() ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  VkResult result = build(frag, r_.scene_format(), r_.samples_, true, true, 0, opaque_,
                          VK_NULL_HANDLE, nullptr, false, VK_NULL_HANDLE, cull_back);
  if (result == VK_SUCCESS)
    result = build(frag, r_.scene_format(), r_.samples_, true, false, 0, opaque_after_z_,
                   VK_NULL_HANDLE, nullptr, false, VK_NULL_HANDLE, cull_back, true);
  if (result == VK_SUCCESS)
    result = build(VK_NULL_HANDLE, r_.scene_format(), r_.samples_, true, true, 0, car_z_prepass_,
                   VK_NULL_HANDLE, nullptr, false, VK_NULL_HANDLE, cull_back, false, false);
  if (result == VK_SUCCESS) result = build(frag, r_.scene_format(), r_.samples_, true, false, 1, glass_);
  if (result == VK_SUCCESS) result = build(frag, r_.scene_format(), r_.samples_, true, false, 2, glass_mul_);
  if (result == VK_SUCCESS) result = build(frag, r_.scene_format(), r_.samples_, true, false, 3, glass_add_);
  if (result == VK_SUCCESS && !guest_)
    result = build(frag, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, true, true, 0, env_car_opaque_);
  if (result == VK_SUCCESS)
    result = build(mask, VK_FORMAT_R8_UNORM, r_.samples_, false, false, 0, mask_);
  // Under-car contact / drop shadow pipeline (drive mode multiply-blended quad).
  if (result == VK_SUCCESS && !guest_) {
    VkPushConstantRange cs_push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo cs_pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cs_pl.setLayoutCount = 1;
    cs_pl.pSetLayouts = &frame_layout_;
    cs_pl.pushConstantRangeCount = 1;
    cs_pl.pPushConstantRanges = &cs_push;
    result = vkCreatePipelineLayout(r_.device_, &cs_pl, nullptr, &contact_shadow_pipe_layout_);
    if (result == VK_SUCCESS) {
      VkShaderModule cs_v = VK_NULL_HANDLE, cs_f = VK_NULL_HANDLE;
      result = make_module(kContact_shadow_vert_spv, sizeof(kContact_shadow_vert_spv), cs_v, "contact_shadow.vert");
      if (result == VK_SUCCESS)
        result = make_module(kContact_shadow_frag_spv, sizeof(kContact_shadow_frag_spv), cs_f, "contact_shadow.frag");
      if (result == VK_SUCCESS) {
        VkPipelineVertexInputStateCreateInfo empty_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, cs_v, "main",
             nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, cs_f, "main",
             nullptr}};
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = r_.samples_;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bl.attachmentCount = 1;
        bl.pAttachments = &att;
        const VkFormat sc_fmt = r_.scene_format();
        VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rend.colorAttachmentCount = 1;
        rend.pColorAttachmentFormats = &sc_fmt;
        rend.depthAttachmentFormat = r_.depth_format_;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &rend;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &empty_vi;
        pci.pInputAssemblyState = &assembly;
        pci.pViewportState = &viewport;
        pci.pRasterizationState = &raster_bias;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &bl;
        pci.pDynamicState = &dynamic_bias;
        pci.layout = contact_shadow_pipe_layout_;
        result = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &pci, nullptr, &contact_shadow_pipe_);
      }
      if (cs_v) vkDestroyShaderModule(r_.device_, cs_v, nullptr);
      if (cs_f) vkDestroyShaderModule(r_.device_, cs_f, nullptr);
    }
  }
  // The textured world (world.vert / world.frag + discard-free world_opaque.frag):
  // opaque (Early-Z), dithered opaque, alpha to coverage, and blended with depth writes off.
  VkShaderModule world_vert = VK_NULL_HANDLE, world_frag = VK_NULL_HANDLE, world_opaque_frag = VK_NULL_HANDLE;
  if (result == VK_SUCCESS) result = make_module(kWorld_vert_spv, sizeof(kWorld_vert_spv), world_vert, "world.vert");
  if (result == VK_SUCCESS) result = make_module(kWorld_frag_spv, sizeof(kWorld_frag_spv), world_frag, "world.frag");
  if (result == VK_SUCCESS)
    result = make_module(kWorld_opaque_frag_spv, sizeof(kWorld_opaque_frag_spv), world_opaque_frag, "world_opaque.frag");
  VkVertexInputBindingDescription world_binding{0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription world_attributes[7] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
      {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(WorldVertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv0)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv1)},
      {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldVertex, colour)},
      {5, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(WorldVertex, centre)},
      {6, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(WorldVertex, uv2)}};
  VkPipelineVertexInputStateCreateInfo world_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  world_input.vertexBindingDescriptionCount = 1;
  world_input.pVertexBindingDescriptions = &world_binding;
  world_input.vertexAttributeDescriptionCount = 7;
  world_input.pVertexAttributeDescriptions = world_attributes;
  if (result == VK_SUCCESS)
    result = build(world_opaque_frag, r_.scene_format(), r_.samples_, true, true, 0, world_opaque_, world_vert,
                   &world_input, false, world_pipeline_layout_, cull_back);
  if (result == VK_SUCCESS)
    result = build(world_opaque_frag, r_.scene_format(), r_.samples_, true, false, 0, world_opaque_after_z_, world_vert,
                   &world_input, false, world_pipeline_layout_, cull_back, true);
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, true, 0, world_opaque_dither_, world_vert,
                   &world_input, false, world_pipeline_layout_, cull_back);
  if (result == VK_SUCCESS)
    result = build(VK_NULL_HANDLE, r_.scene_format(), r_.samples_, true, true, 0, world_z_prepass_, world_vert,
                   &world_input, false, world_pipeline_layout_, cull_back, false, false);
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, true, 0, world_coverage_, world_vert,
                   &world_input, true, world_pipeline_layout_);
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, false, 1, world_alpha_, world_vert,
                   &world_input, false, world_pipeline_layout_);
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, false, 4, world_water_, world_vert,
                   &world_input, false, world_pipeline_layout_);
  if (result == VK_SUCCESS && !guest_)
    result = build(world_opaque_frag, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, true, true, 0,
                   env_world_opaque_, world_vert, &world_input, false, world_pipeline_layout_, cull_back);
  if (result == VK_SUCCESS && !guest_)
    result = build(world_frag, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, true, true, 0,
                   env_world_coverage_, world_vert, &world_input, true, world_pipeline_layout_);
  // The sun shadow pass (ours): depth only into the shadow map, biased;
  // opaque casters with no fragment shader, cut-outs alpha-tested.
  VkShaderModule shadow_frag = VK_NULL_HANDLE;
  auto build_shadow = [&](VkShaderModule vertex, VkShaderModule fragment,
                          const VkPipelineVertexInputStateCreateInfo* input, VkPipelineLayout pipe_layout,
                          VkCullModeFlags cull_mode, VkPipeline& out) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment,
         "main", nullptr}};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineRasterizationStateCreateInfo biased = raster;
    biased.cullMode = cull_mode;
    if (vertex == world_vert) biased.frontFace = VK_FRONT_FACE_CLOCKWISE;
    biased.depthBiasEnable = VK_TRUE;
    biased.depthBiasConstantFactor = 2.0f;
    biased.depthBiasSlopeFactor = 2.5f;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.depthAttachmentFormat = Renderer::kShadowFormat;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = fragment ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &biased;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipe_layout;
    return vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &info, nullptr, &out);
  };
  if (result == VK_SUCCESS && r_.options_.shadow.size > 0) {
    result = make_module(kShadow_frag_spv, sizeof(kShadow_frag_spv), shadow_frag, "shadow.frag");
    if (result == VK_SUCCESS)
      result = build_shadow(world_vert, VK_NULL_HANDLE, &world_input, world_pipeline_layout_, cull_back, shadow_world_);
    if (result == VK_SUCCESS)
      result = build_shadow(world_vert, shadow_frag, &world_input, world_pipeline_layout_, VK_CULL_MODE_NONE, shadow_cutout_);
    if (result == VK_SUCCESS)
      result = build_shadow(vert, VK_NULL_HANDLE, &vertex_input, pipeline_layout_, cull_back, shadow_car_);
  }
  if (shadow_frag) vkDestroyShaderModule(r_.device_, shadow_frag, nullptr);
  if (world_vert) vkDestroyShaderModule(r_.device_, world_vert, nullptr);
  if (world_frag) vkDestroyShaderModule(r_.device_, world_frag, nullptr);
  if (world_opaque_frag) vkDestroyShaderModule(r_.device_, world_opaque_frag, nullptr);
  vkDestroyShaderModule(r_.device_, vert, nullptr);
  vkDestroyShaderModule(r_.device_, frag, nullptr);
  vkDestroyShaderModule(r_.device_, mask, nullptr);
  VK_TRY(result, "showroom pipelines");
  return true;
}

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
  if (!r_.upload_buffer(vertices_.data(), vertices_.size() * sizeof(bake::Vertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rt_geom_flags, vertex_buffer_) ||
      !r_.upload_buffer(indices_.data(), indices_.size() * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt_geom_flags, index_buffer_) ||
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
    gs.index_capacity = gs.vertex_capacity * 3;
    gs.retire = [this](std::function<void()> f) { retire(std::move(f)); };
    auto gw = std::make_unique<WorldGpu>(r_);
    std::string gerr;
    if (gw->init(gs, gerr)) {
      gpu_world_ = std::move(gw);
      if (auto hv = gpu_world_->hiz_raw_view()) r_.set_hiz_preview_view(hv);
    } else {
      notes_.push_back("GPU-driven world off: " + gerr);
    }
  }
  if (guest_) return true;
  SceneHooks hooks;
  hooks.before = [this](VkCommandBuffer cmd) {
    ++world_frame_;
    release_retired();
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
        for (const auto& in : instances_) {
          if (in.car != g || written >= kMaxCarInstances) continue;
          CarInstanceGpu& ci = dst[written++];
          std::memcpy(ci.model, in.model, sizeof ci.model);
          for (int k = 0; k < 4; ++k) ci.stripe[k] = g->stripe_[k];
          const drive::LampShaderVectors& active_lamps = in.has_lamps ? in.lamps : g->lamp_vectors_;
          for (uint32_t ch = 0; ch < 30; ++ch) {
            if (ch == 0) {
              ci.lamp_rgb[ch][0] = ci.lamp_rgb[ch][1] = ci.lamp_rgb[ch][2] = ci.lamp_rgb[ch][3] = 0.0f;
            } else {
              const auto rgb = drive::evaluate_lamp_emission(static_cast<drive::LampChannel>(ch), active_lamps,
                                                             g->lamp_colors_);
              ci.lamp_rgb[ch][0] = rgb[0], ci.lamp_rgb[ch][1] = rgb[1], ci.lamp_rgb[ch][2] = rgb[2],
              ci.lamp_rgb[ch][3] = 1.0f;
            }
          }
        }
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
      if (pipe->init(bl_layout, sampler_, fb_white, rt_err)) {
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

void Showroom::set_background(float r, float g, float b) {
  r_.scene_.clear[0] = r;
  r_.scene_.clear[1] = g;
  r_.scene_.clear[2] = b;
  r_.scene_.clear[3] = 1.0f;
}

void Showroom::set_world_overlay(std::span<const WorldVertex> triangles) {
  const size_t n = std::min(triangles.size(), kMaxOverlayVertices);
  const size_t tri_n = (n / 3) * 3;
  overlay_cpu_.assign(triangles.begin(), triangles.begin() + ptrdiff_t(tri_n));
  overlay_count_ = uint32_t(tri_n);
  overlay_dirty_ = true;
}

void Showroom::update_frame(VkCommandBuffer cmd) {
  update_buffer(cmd, frame_buffer_.buffer, frame_);
  if (overlay_dirty_ && overlay_vertices_.buffer) {
    overlay_dirty_ = false;
    if (!overlay_cpu_.empty()) {
      VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
      b.srcAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = overlay_vertices_.buffer;
      b.size = VK_WHOLE_SIZE;
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
      vkCmdUpdateBuffer(cmd, overlay_vertices_.buffer, 0, overlay_cpu_.size() * sizeof(WorldVertex),
                        overlay_cpu_.data());
      b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
      b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
  }
}

void Showroom::update_buffer(VkCommandBuffer cmd, VkBuffer buffer, const ShowroomFrame& f) {
  VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = buffer;
  b.size = VK_WHOLE_SIZE;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.bufferMemoryBarrierCount = 1;
  dep.pBufferMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
  ShowroomFrame copy = f;
  for (int k = 0; k < 4; ++k) copy.cockpit_mfd[k] = cockpit_mfd_[k];
  vkCmdUpdateBuffer(cmd, buffer, 0, sizeof(ShowroomFrame), &copy);
  b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void Showroom::preview_colour(size_t index) {
  const auto& colours = car_.car.colours;
  if (index >= colours.size()) return;
  options_.colour = index;
  const bake::Colour& c = colours[index];
  const float rgb[3] = {srgb_to_linear(float((c.rgb >> 16) & 0xFF) / 255.0f),
                        srgb_to_linear(float((c.rgb >> 8) & 0xFF) / 255.0f), srgb_to_linear(float(c.rgb & 0xFF) / 255.0f)};
  if (c.has_stripe) {
    stripe_[0] = srgb_to_linear(float((c.stripe_rgb >> 16) & 0xFF) / 255.0f);
    stripe_[1] = srgb_to_linear(float((c.stripe_rgb >> 8) & 0xFF) / 255.0f);
    stripe_[2] = srgb_to_linear(float(c.stripe_rgb & 0xFF) / 255.0f);
  }
  for (auto& d : draws_) {
    if (uint32_t(d.push.params[0] + 0.5f) != uint32_t(MaterialClass::kPaint)) continue;
    uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
    for (int k = 0; k < 3; ++k) d.push.tint[k] = rgb[k];
    flags = c.metallic ? flags | kFlagMetallic : flags & ~kFlagMetallic;
    flags = c.has_stripe && d.stripe_able ? flags | kFlagStripe : flags & ~kFlagStripe;
    d.push.params[1] = float(flags);
  }
}

void Showroom::set_paint_spec(const cars::CarPaintSpec& paint) {
  auto to_lin = [](uint32_t rgb) -> std::array<float, 3> {
    return {srgb_to_linear(float((rgb >> 16) & 0xFF) / 255.0f),
            srgb_to_linear(float((rgb >> 8) & 0xFF) / 255.0f),
            srgb_to_linear(float(rgb & 0xFF) / 255.0f)};
  };
  if (paint.body.active) {
    const auto sec = to_lin(paint.body.sec_rgb);
    paint_two_tone_[0] = sec[0];
    paint_two_tone_[1] = sec[1];
    paint_two_tone_[2] = sec[2];
    paint_two_tone_[3] = paint.body.two_tone_power;
    paint_finish_[0] = float(int(paint.body.finish));
    paint_finish_[1] = paint.body.two_tone_scale;
    paint_finish_[2] = paint.body.two_tone_bias;
  }
  paint_finish_[3] = std::clamp(paint.window_tint, 0.0f, 1.0f);
  for (int i = 0; i < 4; ++i) {
    frame_.paint_two_tone[i] = paint_two_tone_[i];
    frame_.paint_finish[i] = paint_finish_[i];
  }

  for (auto& d : draws_) {
    const auto klass = MaterialClass(uint32_t(d.push.params[0] + 0.5f));
    if (klass == MaterialClass::kPaint) {
      const cars::PaintRegionSpec* reg = &paint.body;
      if (paint.hood.active && d.part_name.rfind("hood", 0) == 0) reg = &paint.hood;
      else if (paint.wing.active && d.part_name.rfind("wing", 0) == 0) reg = &paint.wing;
      else if (paint.mirrors.active && d.part_name.find("mirror") != std::string::npos) reg = &paint.mirrors;
      if (reg->active) {
        const auto lin = to_lin(reg->rgb);
        for (int k = 0; k < 3; ++k) d.push.tint[k] = lin[k];
        uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
        const bool is_met = (reg->finish == cars::PaintFinish::kMetallic || reg->finish == cars::PaintFinish::kTwoTone);
        flags = is_met ? (flags | kFlagMetallic) : (flags & ~kFlagMetallic);
        flags &= ~kFlagRaceLivery;
        d.push.params[1] = float(flags);
      }
    } else if (klass == MaterialClass::kRim && paint.rims.active && !options_.visual.wheel_is_chrome) {
      const auto lin = to_lin(paint.rims.rgb);
      for (int k = 0; k < 3; ++k) d.push.tint[k] = lin[k];
    } else if (klass == MaterialClass::kCaliper && paint.calipers.active) {
      const auto lin = to_lin(paint.calipers.rgb);
      for (int k = 0; k < 3; ++k) d.push.tint[k] = lin[k];
    }
  }
}

bool Showroom::set_custom_livery(const uint8_t* rgba, uint32_t width, uint32_t height, std::string& error) {
  if (!rgba || width == 0 || height == 0 || !r_.device_) {
    error = "invalid custom livery or renderer not initialized";
    return false;
  }
  r_.wait_idle();
  r_.destroy_image(custom_livery_img_);

  bake::Texture tex;
  tex.name = "@custom_livery";
  tex.format = bake::TextureFormat::kRgba8;
  tex.levels.push_back({width, height, std::vector<uint8_t>(rgba, rgba + size_t(width) * height * 4u)});
  if (!upload_texture(&tex, true, custom_livery_img_, error)) return false;

  // Bind the custom 1024^2 RGBA8 livery texture into binding 2 of every paint draw's material set.
  std::set<uint32_t> updated_sets;
  for (auto& d : draws_) {
    if (MaterialClass(uint32_t(d.push.params[0] + 0.5f)) != MaterialClass::kPaint) continue;
    uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
    flags |= kFlagLivery;
    d.push.params[1] = float(flags);
    if (d.descriptor < material_sets_.size() && updated_sets.insert(d.descriptor).second) {
      VkDescriptorImageInfo info{sampler_, custom_livery_img_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = material_sets_[d.descriptor];
      write.dstBinding = 2;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &info;
      vkUpdateDescriptorSets(r_.device_, 1, &write, 0, nullptr);
    }
  }
  custom_livery_active_ = true;
  frame_.options[2] = 2.0f;
  return true;
}

void Showroom::set_damage_state(const drive::CarDamageState& dmg) {
  if (dmg.mode == drive::DamageMode::kNone) {
    for (int i = 0; i < 4; ++i) damage_dirt_[i] = damage_zones_[i] = 0.0f;
  } else {
    damage_dirt_[0] = std::clamp(dmg.body_dirt, 0.0f, 1.0f);
    damage_dirt_[1] = std::clamp(dmg.tyre_dirt, 0.0f, 1.0f);
    damage_dirt_[2] = std::clamp(dmg.glass_crack, 0.0f, 1.0f);
    damage_dirt_[3] = std::clamp(dmg.scratch_amount, 0.0f, 1.0f);
    damage_zones_[0] = std::clamp(dmg.zone_front, 0.0f, 1.0f);
    damage_zones_[1] = std::clamp(dmg.zone_rear, 0.0f, 1.0f);
    damage_zones_[2] = std::clamp(dmg.zone_left, 0.0f, 1.0f);
    damage_zones_[3] = std::clamp(dmg.zone_right, 0.0f, 1.0f);
  }
  for (int i = 0; i < 4; ++i) {
    frame_.damage_dirt[i] = damage_dirt_[i];
    frame_.damage_zones[i] = damage_zones_[i];
  }
}

void Showroom::record_draws(VkCommandBuffer cmd, VkPipeline opaque, VkPipeline glass, int pass_mode) {
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  uint32_t bound_desc = UINT32_MAX;
  auto draw = [&](const Draw& d) {
    if (d.descriptor != bound_desc) {
      bound_desc = d.descriptor;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                              &material_sets_[d.descriptor], 0, nullptr);
    }
    ShowroomPush push = d.push;
    if (d.lamp_channel != drive::LampChannel::kNone && opaque != glass) {
      const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, lamp_vectors_, lamp_colors_);
      if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
        push.tint[0] = rgb[0];
        push.tint[1] = rgb[1];
        push.tint[2] = rgb[2];
        push.params[1] = float(uint32_t(push.params[1] + 0.5f) | kFlagLampEmissive);
      }
    }
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  };
  if (pass_mode != 2) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
    r_.apply_vrs(cmd);
    for (const auto& d : draws_)
      if (!d.glass || opaque == glass) {
        draw(d);
        ++car_draws_;
      }
    if (opaque == glass) return;
    if (!guest_) r_.mark(cmd, "car_opaque");
    if (pass_mode == 1) return;
  } else if (opaque == glass) {
    return;
  }
  // Glass last, far to near from the eye.
  std::vector<const Draw*> sorted;
  for (const auto& d : draws_)
    if (d.glass) sorted.push_back(&d);
  auto distance = [&](const Draw* d) {
    float s = 0;
    for (int k = 0; k < 3; ++k) {
      const float e = d->centre[size_t(k)] - frame_.eye[k];
      s += e * e;
    }
    return s;
  };
  std::stable_sort(sorted.begin(), sorted.end(),
                   [&](const Draw* a, const Draw* b) { return distance(a) > distance(b); });
  VkPipeline bound_glass = VK_NULL_HANDLE;
  for (const Draw* d : sorted) {
    if (d->is_cover && glass_mul_ && glass_add_) {
      // FH1 two-pass detail_glass_red/amber/clear lamp cover (SHOWROOM_LIGHTING.md:187):
      // Pass 1 Multiply transmittance with LightsSampler + GlassColor,
      // Pass 2 Additive Fresnel env reflection + sun specular.
      if (d->descriptor != bound_desc) {
        bound_desc = d->descriptor;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                                &material_sets_[d->descriptor], 0, nullptr);
      }
      ShowroomPush push = d->push;
      if (bound_glass != glass_mul_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_glass = glass_mul_);
      push.params[2] = 7.0f;
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d->index_count, 1, d->first_index, d->vertex_offset, 0);

      if (bound_glass != glass_add_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_glass = glass_add_);
      push.params[2] = 8.0f;
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d->index_count, 1, d->first_index, d->vertex_offset, 0);
      car_draws_ += 2;
    } else {
      if (bound_glass != glass) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_glass = glass);
      draw(*d);
      ++car_draws_;
    }
  }
}

void Showroom::bind_dynamic_env(VkImageView cube_view, VkSampler sampler) {
  if (!r_.device_ || !frame_set_ || !cube_view || !sampler || bound_dyn_env_view_ == cube_view) return;
  // Allocate a fresh frame descriptor set from pool_ so we NEVER mutate an in-flight set
  // or call vkDeviceWaitIdle() on traffic/race car spawn!
  VkDescriptorSet fresh = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &frame_layout_;
  if (vkAllocateDescriptorSets(r_.device_, &ai, &fresh) == VK_SUCCESS && fresh) {
    VkDescriptorBufferInfo buffer{frame_buffer_.buffer, 0, sizeof(ShowroomFrame)};
    VkDescriptorImageInfo cube{sampler, cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo static_cube{sampler_, environment_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map{r_.shadow_sampler_, r_.shadow_map_.view,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map_raw{r_.shadow_raw_sampler_, r_.shadow_map_.view,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo hl_buf{headlights_ubo_.buffer, 0, sizeof(HeadlightUbo)};
    const VkImageView mirror_view = (dyn_env_ready_ && mirror_tex_.view) ? mirror_tex_.view : gpu_textures_[1].view;
    VkDescriptorImageInfo mir{sampler_, mirror_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo inst_buf{guest_ ? empty_inst_ssbo_.buffer : car_inst_buf_.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo dirt_p{sampler_, shared_dirt_pattern_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dirt_m{sampler_, shared_dirt_mask_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo gcrack{sampler_, shared_glass_crack_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[12]{};
    for (int i = 0; i < 12; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = fresh;
      writes[i].dstBinding = uint32_t(i);
      writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[0].pBufferInfo = &buffer;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = &cube;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[2].pImageInfo = &map;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[3].pImageInfo = &cookie;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[4].pBufferInfo = &hl_buf;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[5].pImageInfo = &mir;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[6].pBufferInfo = &inst_buf;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[7].pImageInfo = &static_cube;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[8].pImageInfo = &dirt_p;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[9].pImageInfo = &dirt_m;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[10].pImageInfo = &gcrack;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[11].pImageInfo = &map_raw;
    vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
    frame_set_ = fresh;
    bound_dyn_env_view_ = cube_view;
    return;
  }
  bound_dyn_env_view_ = cube_view;
}

bool Showroom::reload_car(const std::filesystem::path& bake_dir, const std::string& car,
                          const ShowroomOptions& options, std::string& error) {
  if (!r_.device_ || !sampler_ || !material_layout_) {
    error = "showroom not uploaded";
    return false;
  }
  // Load new car bake on CPU first.
  if (!load(bake_dir, car, options, error)) return false;

  // Upload new car vertex & index buffers.
  const VkBufferUsageFlags rt_geom_flags =
      r_.rt_supported()
          ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
          : 0u;
  Renderer::Buffer new_vb, new_ib;
  if (!r_.upload_buffer(vertices_.data(), vertices_.size() * sizeof(bake::Vertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rt_geom_flags, new_vb) ||
      !r_.upload_buffer(indices_.data(), indices_.size() * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt_geom_flags, new_ib)) {
    r_.destroy_buffer(new_vb);
    r_.destroy_buffer(new_ib);
    error = "reload_car geometry buffers";
    return false;
  }

  // Upload new car textures [2..N] in one single batch; preserve [0] (@white) and [1] (@black)
  // because WorldGpu bindless slots 0/1 and frame_set_ mirror fallback reference their views.
  std::vector<GpuTexture> new_gpu_textures(textures_.size());
  std::vector<TextureUploadItem> batch;
  if (textures_.size() > 2) batch.reserve(textures_.size() - 2);
  for (size_t i = 2; i < textures_.size(); ++i) {
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
    batch.push_back({t, srgb, &new_gpu_textures[i], true});
  }
  if (!upload_textures_batch(batch, error)) {
    r_.destroy_buffer(new_vb);
    r_.destroy_buffer(new_ib);
    return false;
  }
  if (gpu_textures_.size() >= 2 && new_gpu_textures.size() >= 2) {
    new_gpu_textures[0] = gpu_textures_[0];
    new_gpu_textures[1] = gpu_textures_[1];
  }

  // Create fresh material descriptor pool & sets for the new car draws.
  for (auto& d : draws_) {
    auto [it, inserted] = descriptor_by_textures_.emplace(d.textures, uint32_t(descriptor_by_textures_.size()));
    d.descriptor = it->second;
  }
  const uint32_t sets = uint32_t(descriptor_by_textures_.size());
  const VkDescriptorPoolSize mat_sizes[1] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(1u, sets) * 5}};
  VkDescriptorPoolCreateInfo mat_pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  mat_pool_ci.maxSets = std::max(1u, sets);
  mat_pool_ci.poolSizeCount = 1;
  mat_pool_ci.pPoolSizes = mat_sizes;
  VkDescriptorPool new_mat_pool = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(r_.device_, &mat_pool_ci, nullptr, &new_mat_pool) != VK_SUCCESS) {
    for (size_t i = 2; i < new_gpu_textures.size(); ++i) r_.destroy_image(new_gpu_textures[i]);
    r_.destroy_buffer(new_vb);
    r_.destroy_buffer(new_ib);
    error = "reload_car material pool";
    return false;
  }
  std::vector<VkDescriptorSet> new_mat_sets(sets);
  if (sets > 0) {
    std::vector<VkDescriptorSetLayout> layouts(sets, material_layout_);
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = new_mat_pool;
    alloc.descriptorSetCount = sets;
    alloc.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(r_.device_, &alloc, new_mat_sets.data()) != VK_SUCCESS) {
      vkDestroyDescriptorPool(r_.device_, new_mat_pool, nullptr);
      for (size_t i = 2; i < new_gpu_textures.size(); ++i) r_.destroy_image(new_gpu_textures[i]);
      r_.destroy_buffer(new_vb);
      r_.destroy_buffer(new_ib);
      error = "reload_car material sets";
      return false;
    }
  }
  for (const auto& [textures, index] : descriptor_by_textures_) {
    VkDescriptorImageInfo images[5];
    VkWriteDescriptorSet writes[5];
    for (uint32_t b = 0; b < 5; ++b) {
      VkImageView view = VK_NULL_HANDLE;
      if (b < 3) {
        view = new_gpu_textures[textures[b]].view;
      } else if (b == 3) {
        view = (textures[3] != UINT32_MAX && textures[3] < new_gpu_textures.size())
                   ? new_gpu_textures[textures[3]].view
                   : flat_normal_tex_.view;
      } else {
        view = (textures[4] != UINT32_MAX && textures[4] < new_gpu_textures.size())
                   ? new_gpu_textures[textures[4]].view
                   : new_gpu_textures[textures[0]].view;
      }
      images[b] = {sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[b].dstSet = new_mat_sets[index];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(r_.device_, 5, writes, 0, nullptr);
  }

  // Swap live car GPU resources and retire the previous car's GPU objects off-thread (zero vkDeviceWaitIdle!).
  Renderer::Buffer old_vb = vertex_buffer_;
  Renderer::Buffer old_ib = index_buffer_;
  VkDescriptorPool old_mat_pool = material_pool_;
  GpuTexture old_livery = custom_livery_img_;
  std::vector<GpuTexture> old_car_textures;
  if (gpu_textures_.size() > 2) {
    old_car_textures.assign(gpu_textures_.begin() + 2, gpu_textures_.end());
  }

  vertex_buffer_ = new_vb;
  index_buffer_ = new_ib;
  material_pool_ = new_mat_pool;
  material_sets_ = std::move(new_mat_sets);
  gpu_textures_ = std::move(new_gpu_textures);
  custom_livery_img_ = {};
  custom_livery_active_ = false;
  lamp_points_.clear();
  lamp_state_active_ = false;
  lamp_vectors_ = {};
  driver_wheel_rad_ = 0.0f;
  // Reset per-frame car-derived uniforms so the new car's stripe/paint finish take effect immediately.
  for (int i = 0; i < 4; ++i) {
    frame_.stripe[i] = stripe_[i];
    frame_.paint_two_tone[i] = paint_two_tone_[i];
    frame_.paint_finish[i] = paint_finish_[i];
    frame_.damage_dirt[i] = damage_dirt_[i];
    frame_.damage_zones[i] = damage_zones_[i];
  }
  frame_.options[2] = 0.0f;

  retire([this, old_vb, old_ib, old_mat_pool, old_livery,
          old_tex = std::move(old_car_textures)]() mutable {
    if (old_mat_pool) vkDestroyDescriptorPool(r_.device_, old_mat_pool, nullptr);
    for (auto& t : old_tex) r_.destroy_image(t);
    r_.destroy_image(old_livery);
    r_.destroy_buffer(old_vb);
    r_.destroy_buffer(old_ib);
  });
  return true;
}

void Showroom::set_instances(std::vector<Instance> list) {
  instances_ = std::move(list);
  if (dyn_env_ready_ && dyn_env_.view && sampler_) {
    for (const auto& in : instances_) {
      if (in.car && in.car->bound_dyn_env_view_ != dyn_env_.view) {
        in.car->bind_dynamic_env(dyn_env_.view, sampler_);
      }
    }
  }
}

void Showroom::add_instances(const std::vector<Instance>& more) {
  instances_.insert(instances_.end(), more.begin(), more.end());
  if (dyn_env_ready_ && dyn_env_.view && sampler_) {
    for (const auto& in : more) {
      if (in.car && in.car->bound_dyn_env_view_ != dyn_env_.view) {
        in.car->bind_dynamic_env(dyn_env_.view, sampler_);
      }
    }
  }
}

void Showroom::record_contact_shadows(VkCommandBuffer cmd) {
  if (!world_lit_ || !contact_shadow_pipe_ || !contact_shadow_pipe_layout_ || !frame_set_) return;
  struct ContactPush {
    float model[16];
    float params[4];
    float wheels_uv[4];
  };
  static_assert(sizeof(ContactPush) == 96);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, contact_shadow_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, contact_shadow_pipe_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  const float sign = r_.reverse_z() ? 1.0f : -1.0f;
  vkCmdSetDepthBias(cmd, sign * 96.0f, 0.0f, sign * 1.5f);
  const float night_amt = tod_active_ ? std::clamp(tod_state_.switch_on_lights, 0.0f, 1.0f) : 0.0f;
  // Keep cars grounded cleanly across bright sun, shade, overcast, and night on all 4 hardware tiers.
  const float min_dark = 0.14f + 0.10f * night_amt;

  auto draw_one = [&](const Showroom* car_sr, const float* car_model, const std::array<float, 3>& bmin,
                      const std::array<float, 3>& bmax, float strength) {
    const float hx = std::max(0.75f, 0.5f * (bmax[0] - bmin[0])) * 1.18f;
    const float hz = std::max(1.65f, 0.5f * (bmax[2] - bmin[2])) * 1.10f;
    const float cx = 0.5f * (bmin[0] + bmax[0]);
    const float cz = 0.5f * (bmin[2] + bmax[2]);
    const float cy = bmin[1] + 0.025f;

    // Derive per-car normalized wheel contact patch coordinates & suspension clearance from live poses/rest hubs.
    float f_u = 0.58f, f_v = -0.54f, r_u = 0.58f, r_v = 0.54f;
    float clearance = 0.25f;
    if (car_sr) {
      const auto& rest_w = car_sr->car_.car.wheel_centres;
      const auto& rest_b = car_sr->car_.car.wheel_centres_body;
      float fl_x = std::fabs(rest_w[0][0]), fr_x = std::fabs(rest_w[1][0]);
      float rl_x = std::fabs(rest_w[2][0]), rr_x = std::fabs(rest_w[3][0]);
      if (fl_x + fr_x > 0.2f && rl_x + rr_x > 0.2f) {
        f_u = std::clamp(0.5f * (fl_x + fr_x) / hx, 0.35f, 0.84f);
        r_u = std::clamp(0.5f * (rl_x + rr_x) / hx, 0.35f, 0.84f);
        const float fz = 0.5f * (rest_w[0][2] + rest_w[1][2]);
        const float rz = 0.5f * (rest_w[2][2] + rest_w[3][2]);
        f_v = std::clamp((fz - cz) / hz, -0.82f, 0.82f);
        r_v = std::clamp((rz - cz) / hz, -0.82f, 0.82f);
      }
      if (car_sr->have_wheel_poses_) {
        float avg_drop = 0.0f;
        for (int w = 0; w < 4; ++w) {
          // Lower wheel hub relative to rest body frame => chassis lifted higher off the road.
          avg_drop += (rest_b[size_t(w)][1] - car_sr->wheel_poses_[w].centre[1]);
        }
        avg_drop *= 0.25f;
        clearance = std::clamp(0.25f + avg_drop * 2.4f, 0.0f, 1.0f);
      }
    }

    float local[16] = {hx, 0, 0, 0,  0, 1, 0, 0,  0, 0, hz, 0,  cx, cy, cz, 1};
    ContactPush push{};
    if (car_model) {
      multiply(car_model, local, push.model);
    } else {
      std::memcpy(push.model, local, sizeof push.model);
    }
    push.params[0] = strength;
    push.params[1] = min_dark;
    push.params[2] = 0.24f;  // rounded body footprint corner radius
    push.params[3] = clearance;
    push.wheels_uv[0] = f_u;
    push.wheels_uv[1] = f_v;
    push.wheels_uv[2] = r_u;
    push.wheels_uv[3] = r_v;
    vkCmdPushConstants(cmd, contact_shadow_pipe_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ContactPush), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  };

  if (draw_car_ && !cockpit_camera_active_ && have_bounds_) {
    draw_one(this, nullptr, bounds_min_, bounds_max_, 0.92f);
  }
  for (const auto& in : instances_) {
    if (!in.car || !in.car->have_bounds_) continue;
    draw_one(in.car, in.model, in.car->bounds_min_, in.car->bounds_max_, 0.88f);
  }
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
}

void Showroom::record_z_prepass(VkCommandBuffer cmd) {
  if (world_textured_ && !world_draws_.empty()) {
    if (arena_on() && gpu_world_->table_rows() > 0) {
      WorldGpu::DrawParams dp;
      dp.frame_set = frame_set_;
      std::memcpy(dp.model, world_transform_, sizeof dp.model);
      for (int k = 0; k < 3; ++k) dp.sun[k] = world_sun_[k];
      for (int k = 0; k < 4; ++k) dp.up[k] = world_up_[k];
      dp.bias_sign = r_.reverse_z() ? 1.0f : -1.0f;
      dp.decal_bias_constant = kDecalBiasConstant;
      dp.decal_bias_slope = kDecalBiasSlope;
      gpu_world_->record_z_prepass(cmd, dp);
    } else if (world_z_prepass_) {
      world_bind_buffers(cmd, false);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_z_prepass_);
      for (const auto& d : world_draws_) {
        if (!d.cull || d.blend != 0) continue;
        ShowroomPush push = d.push;
        world_draw_model(d, world_transform_, push.model);
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      }
    }
  }
  if (draw_car_ && car_z_prepass_ && !draws_.empty()) {
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, car_z_prepass_);
    for (const auto& d : draws_) {
      if (d.glass) continue;
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &d.push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
  }
}

void Showroom::record_all_instances(VkCommandBuffer cmd, bool depth_equal, int pass_mode) {
  if (instances_.empty()) return;
  if (!car_inst_buf_.ready() || !frame_set_) {
    if (pass_mode == 2) return;
    for (const auto& in : instances_)
      if (in.car) in.car->record_instance(cmd, in.model, in.has_lamps ? &in.lamps : nullptr);
    return;
  }
  std::vector<Showroom*> groups;
  for (const auto& in : instances_) {
    if (in.car && std::find(groups.begin(), groups.end(), in.car) == groups.end()) groups.push_back(in.car);
  }
  const uint32_t slot_base = r_.frame_slot() * kMaxCarInstances;
  uint32_t offset = 0;
  const VkPipeline op_pipe = (depth_equal && opaque_after_z_) ? opaque_after_z_ : opaque_;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  const int pass_begin = (pass_mode == 2) ? 1 : 0;
  const int pass_end = (pass_mode == 1) ? 1 : 2;
  for (Showroom* g : groups) {
    uint32_t count = 0;
    for (const auto& in : instances_)
      if (in.car == g && offset + count < kMaxCarInstances) ++count;
    if (count == 0 || g->draws_.empty() || g->material_sets_.empty()) continue;
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g->vertex_buffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, g->index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (int pass = pass_begin; pass < pass_end; ++pass) {
      VkPipeline bound_p = VK_NULL_HANDLE;
      uint32_t bound_desc = UINT32_MAX;
      for (const auto& d : g->draws_) {
        if (d.glass != (pass == 1)) continue;
        ShowroomPush push = d.push;
        uint32_t flags = uint32_t(push.params[1] + 0.5f) | kFlagInstanced;
        flags |= (uint32_t(d.lamp_channel) & 31u) << 14u;
        push.params[1] = float(flags);
        if (d.descriptor != bound_desc) {
          bound_desc = d.descriptor;
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                                  &g->material_sets_[d.descriptor], 0, nullptr);
        }
        if (pass == 1 && d.is_cover && glass_mul_ && glass_add_) {
          if (bound_p != glass_mul_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_mul_);
          push.params[2] = 7.0f;
          vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(ShowroomPush), &push);
          vkCmdDrawIndexed(cmd, d.index_count, count, d.first_index, d.vertex_offset, slot_base + offset);
          if (bound_p != glass_add_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_add_);
          push.params[2] = 8.0f;
          vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(ShowroomPush), &push);
          vkCmdDrawIndexed(cmd, d.index_count, count, d.first_index, d.vertex_offset, slot_base + offset);
        } else {
          VkPipeline want = pass ? g->glass_ : op_pipe;
          if (bound_p != want) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = want);
            if (!pass) r_.apply_vrs(cmd);
          }
          vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(ShowroomPush), &push);
          vkCmdDrawIndexed(cmd, d.index_count, count, d.first_index, d.vertex_offset, slot_base + offset);
        }
      }
    }
    offset += count;
  }
}

bool Showroom::shadows_on() const {
  return shadow_set_ && shadow_world_ && r_.shadow_size_ > 1 && world_textured_ && !world_draws_.empty();
}

void Showroom::place_shadow() {
  const ShadowSettings& s = r_.options_.shadow;
  const auto pcss_q = r_.graphics_settings().shadow_pcss;
  const bool three_cascades = (r_.shadow_cascades_ >= 3u) && (shadow_set_mid_ != VK_NULL_HANDLE) &&
                              (shadow_set_far_ != VK_NULL_HANDLE);
  // Phase B 3-cascade stabilized D32 CSM radii (Near ~20m, Mid ~90m, Far ~320m) on high/ultra;
  // falls back to fast 2-cascade (Near ~30m, Far ~185m) on low/low-power.
  const float near_radius = three_cascades ? 20.0f : std::min(s.radius, 30.0f);
  const float mid_radius = 90.0f;
  const float far_radius = three_cascades ? 320.0f : std::max(s.radius * 3.0f, 185.0f);
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

  std::memcpy(frame_.shadow_matrix, m_near, sizeof m_near);
  std::memcpy(frame_.shadow_matrix_mid, m_mid, sizeof m_mid);
  std::memcpy(frame_.shadow_matrix_far, m_far, sizeof m_far);
  frame_.shadow[0] = 1.0f;
  frame_.shadow[1] = 1.0f / float(r_.shadow_size_);
  frame_.shadow[2] = s.strength;
  frame_.shadow[3] = float(s.taps);
  frame_.shadow_bias[0] = 1.35f * texel_near;
  frame_.shadow_bias[1] = frame_.shadow_bias[2] = frame_.shadow_bias[3] = 0.0f;
  frame_.shadow_mid_bias[0] = 1.45f * texel_mid;
  frame_.shadow_mid_bias[1] = three_cascades ? 1.0f : 0.0f;
  frame_.shadow_mid_bias[2] = (pcss_q == modern::ShadowPcss::kUltra) ? 2.0f
                            : (pcss_q == modern::ShadowPcss::kHigh)  ? 1.0f
                                                                     : 0.0f;
  frame_.shadow_mid_bias[3] = float(r_.shadow_cascades_);
  frame_.shadow_far_bias[0] = 1.55f * texel_far;
  frame_.shadow_far_bias[1] = shadow_set_far_ ? 1.0f : 0.0f;

  auto fill_shadow_frame = [&](ShowroomFrame& dst, const float m[16], const float c[3]) {
    dst = frame_;
    std::memcpy(dst.view_projection, m, 16 * sizeof(float));
    std::memcpy(dst.transparent_view_projection, m, 16 * sizeof(float));
    for (int k = 0; k < 3; ++k) dst.eye[k] = c[k] + sun[k] * 1000.0f;
    dst.eye[3] = 1.0f;
    dst.world_fade[1] = 0.0f;
  };
  fill_shadow_frame(shadow_frame_, m_near, centre_near);
  if (three_cascades) fill_shadow_frame(shadow_frame_mid_, m_mid, centre_mid);
  fill_shadow_frame(shadow_frame_far_, m_far, centre_far);
}

void Showroom::record_shadow(VkCommandBuffer cmd) {
  const uint32_t size = r_.shadow_size_;
  const uint32_t cascades = std::max(1u, r_.shadow_cascades_);
  const uint32_t atlas_w = size > 1 ? size * cascades : size;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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
    for (const auto& d : world_draws_) {
      if (!d.cull || d.blend >= 2 || outside(d) || near_lod_beyond(d, eye)) continue;
      // Far cascade skips tiny billboard/cutout draws unless Ultra PCSS is active for fast 4K framing.
      if (is_far && d.blend == 1 && !ultra_pcss) continue;
      ++shadow_drawn_;
      const VkPipeline pipeline = d.blend == 1 ? shadow_cutout_ : shadow_world_;
      if (pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
      ShowroomPush push = d.push;
      if (d.blend == 1) {
        VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
        if (set) {
          std::lock_guard<std::mutex> lock(world_textures_mutex_);
          world_lru_.touch(d.texture, world_frame_);
        }
        if (!set) {
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
    // Player & traffic cars cast crisp shadows in Near (~20m) and Mid (~90m) cascades.
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
          uint32_t count = 0;
          for (const auto& in : instances_)
            if (in.car == g && offset + count < kMaxCarInstances) ++count;
          if (count > 0 && !g->draws_.empty()) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g->vertex_buffer_.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, g->index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (const auto& d : g->draws_) {
              if (d.glass) continue;
              ShowroomPush push = d.push;
              push.params[1] = float(uint32_t(push.params[1] + 0.5f) | 8192u);
              vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(ShowroomPush), &push);
              vkCmdDrawIndexed(cmd, d.index_count, count, d.first_index, d.vertex_offset, slot_base + offset);
            }
            shadow_cars_ += count;
            offset += count;
          }
        }
      }
    }
  };

  draw_cascade(0, shadow_set_, shadow_frame_, false, true);
  if (cascades >= 3u && shadow_set_mid_ && shadow_set_far_ && atlas_w >= size * 3u) {
    draw_cascade(size, shadow_set_mid_, shadow_frame_mid_, false, true);
    draw_cascade(size * 2u, shadow_set_far_, shadow_frame_far_, true, false);
  } else if (shadow_set_far_ && atlas_w >= size * 2u) {
    draw_cascade(size, shadow_set_far_, shadow_frame_far_, true, false);
  }
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
    if (d.glass) continue;  // glass and lamp covers cast none (ours)
    ShowroomPush push = d.push;
    if (m) multiply(m, d.push.model, push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  }
}

void Showroom::record_instance(VkCommandBuffer cmd, const float m[16], const drive::LampShaderVectors* lamps) {
  if (draws_.empty() || material_sets_.empty()) return;
  const drive::LampShaderVectors& active_lamps = lamps ? *lamps : lamp_vectors_;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  for (int pass = 0; pass < 2; ++pass) {
    VkPipeline bound_p = VK_NULL_HANDLE;
    for (const auto& d : draws_) {
      if (d.glass != (pass == 1)) continue;
      ShowroomPush push = d.push;
      multiply(m, d.push.model, push.model);
      if (d.lamp_channel != drive::LampChannel::kNone) {
        const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, active_lamps, lamp_colors_);
        if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
          push.tint[0] = rgb[0];
          push.tint[1] = rgb[1];
          push.tint[2] = rgb[2];
          push.params[1] = float(uint32_t(push.params[1] + 0.5f) | kFlagLampEmissive);
        }
      }
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                              &material_sets_[d.descriptor], 0, nullptr);
      if (pass == 1 && d.is_cover && glass_mul_ && glass_add_) {
        if (bound_p != glass_mul_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_mul_);
        push.params[2] = 7.0f;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);

        if (bound_p != glass_add_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_add_);
        push.params[2] = 8.0f;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      } else {
        VkPipeline want = pass ? glass_ : opaque_;
        if (bound_p != want) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = want);
          if (!pass) r_.apply_vrs(cmd);
        }
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      }
    }
  }
}

void Showroom::record_instance_offscreen(VkCommandBuffer cmd, VkPipeline car_opaque, VkDescriptorSet view_frame_set,
                                         const float m[16], const drive::LampShaderVectors* lamps) {
  if (draws_.empty() || material_sets_.empty() || !car_opaque || !view_frame_set) return;
  const drive::LampShaderVectors& active_lamps = lamps ? *lamps : lamp_vectors_;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, car_opaque);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &view_frame_set, 0, nullptr);
  for (const auto& d : draws_) {
    if (d.glass) continue;
    ShowroomPush push = d.push;
    multiply(m, d.push.model, push.model);
    if (d.lamp_channel != drive::LampChannel::kNone) {
      const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, active_lamps, lamp_colors_);
      if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
        push.tint[0] = rgb[0];
        push.tint[1] = rgb[1];
        push.tint[2] = rgb[2];
        push.params[1] = float(uint32_t(push.params[1] + 0.5f) | kFlagLampEmissive);
      }
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                            &material_sets_[d.descriptor], 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  }
}

namespace {

int8_t snorm8(float v) { return int8_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }

// The textured world: one vertex per corner, one draw per material, the
// the sky dome first, then opaque materials, then the cut-outs, then the
// blended ones.
WorldGeometry build_textured_world(const WorldTriangles& world) {
  WorldGeometry out;
  out.textured = true;
  out.textures = world.textures;
  {
    // The sky (ours): a unit sphere of directions in the game frame, which
    // world.vert centres on the eye at the far plane and world.frag shades
    // by height.
    WorldGeometry::Draw d;
    d.first_index = 0;
    constexpr int kRings = 12, kSegments = 24;
    constexpr float kPi = 3.14159265358979f;
    auto dir = [&](int ring, int seg) {
      const float a = kPi * float(ring) / kRings - kPi / 2, b = 2 * kPi * float(seg) / kSegments;
      return std::array<float, 3>{std::cos(a) * std::cos(b), std::sin(a), std::cos(a) * std::sin(b)};
    };
    auto vertex = [&](const std::array<float, 3>& p) {
      WorldVertex v{};
      for (int i = 0; i < 3; ++i) v.position[i] = p[size_t(i)];
      out.indices.push_back(uint32_t(out.textured_vertices.size()));
      out.textured_vertices.push_back(v);
    };
    for (int r = 0; r < kRings; ++r)
      for (int s = 0; s < kSegments; ++s) {
        const auto a = dir(r, s), b = dir(r + 1, s), c = dir(r + 1, s + 1), e = dir(r, s + 1);
        vertex(a), vertex(b), vertex(c), vertex(a), vertex(c), vertex(e);
      }
    d.index_count = uint32_t(out.indices.size());
    d.texture = kNoWorldTexture;
    d.blend = 0;
    d.push.params[0] = 0;
    d.push.params[1] = 2;  // world.frag's kFlagSky
    out.draws.push_back(d);
  }
  const size_t n = world.material.size();
  std::vector<std::vector<uint32_t>> by_material(world.materials.size());
  for (uint32_t t = 0; t < n; ++t)
    if (world.material[t] < by_material.size()) by_material[world.material[t]].push_back(t);
  std::vector<uint32_t> order(world.materials.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](uint32_t a, uint32_t b) { return world.materials[a].blend < world.materials[b].blend; });
  out.textured_vertices.reserve(n * 3);
  out.indices.reserve(n * 3);
  // Each material's triangles split by ground cell (by the triangle's
  // first corner), so a draw has a box the view can cull: near scenery in
  // near_cell squares, the far ring in far_cell ones (ours, a balance of
  // draws against culled area: 128 / 768 m cut 4,100 draws but recorded
  // 1,400, which cost the UHD 620 more than the triangles saved;
  // SHIFT_WORLD_CELL="near far" overrides).
  float near_cell = 256, far_cell = 1536;
  if (const char* e = std::getenv("SHIFT_WORLD_CELL")) std::sscanf(e, "%f %f", &near_cell, &far_cell);
  near_cell = std::max(near_cell, 1.0f), far_cell = std::max(far_cell, 1.0f);
  // Within a draw, corners with the same attributes share one vertex, so
  // the post-transform cache can reuse it (SHIFT_WORLD_SHARE=0: one per
  // corner, for comparison).
  struct VertexKey {
    WorldVertex v;
    bool operator==(const VertexKey& o) const { return std::memcmp(&v, &o.v, sizeof v) == 0; }
  };
  struct VertexHash {
    size_t operator()(const VertexKey& k) const {
      const auto* p = reinterpret_cast<const unsigned char*>(&k.v);
      uint64_t h = 1469598103934665603ull;
      for (size_t i = 0; i < sizeof k.v; ++i) h = (h ^ p[i]) * 1099511628211ull;
      return size_t(h);
    }
  };
  std::unordered_map<VertexKey, uint32_t, VertexHash> shared;
  const char* share_env = std::getenv("SHIFT_WORLD_SHARE");
  const bool share = !(share_env && *share_env == '0');
  std::vector<std::pair<int64_t, uint32_t>> cells;  // (cell, triangle), sorted by cell
  for (uint32_t m : order) {
    if (by_material[m].empty()) continue;
    const auto& mat = world.materials[m];
    const bool billboard = mat.layers == 4;
    const bool fade_class = mat.fade != 0;
    // A prop's LOD00 copies (fade class 1) in small cells, so the ones past
    // the LOD band are left out whole on the CPU (near_lod_beyond).
    const float cell = mat.far ? far_cell : mat.fade == 1 ? 64.0f : near_cell;
    cells.clear();
    for (uint32_t t : by_material[m]) {
      const auto& p = world.positions[size_t(t) * 3];
      const int64_t cx = int64_t(std::floor(p[0] / cell)), cz = int64_t(std::floor(p[2] / cell));
      cells.emplace_back(((cx + (int64_t(1) << 20)) << 21) | (cz + (int64_t(1) << 20)), t);
    }
    std::stable_sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // The flat colour of an untextured material: its triangles' surface class.
    std::array<float, 3> tint{0.3f, 0.3f, 0.3f};
    const uint8_t g = world.group[by_material[m][0]];
    if (g < world.colours.size()) tint = world.colours[g];
    for (size_t first = 0; first < cells.size();) {
    size_t last = first;
    while (last < cells.size() && cells[last].first == cells[first].first) ++last;
    WorldGeometry::Draw d;
    d.first_index = uint32_t(out.indices.size());
    d.cull = true;
    for (int i = 0; i < 3; ++i) d.box_min[i] = 1e30f, d.box_max[i] = -1e30f;
    float reach = 0;
    shared.clear();
    const bool normalize_ccw = (mat.blend == 0 && !mat.is_water && !mat.anim_flag && !mat.is_grass && !billboard);
    for (size_t j = first; j < last; ++j) {
      const uint32_t t = cells[j].second;
      int corners[3] = {0, 1, 2};
      if (normalize_ccw) {
        const size_t c0 = size_t(t) * 3;
        const auto& p0 = world.positions[c0];
        const auto& p1 = world.positions[c0 + 1];
        const auto& p2 = world.positions[c0 + 2];
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const float g_x = e1[1] * e2[2] - e1[2] * e2[1];
        const float g_y = e1[2] * e2[0] - e1[0] * e2[2];
        const float g_z = e1[0] * e2[1] - e1[1] * e2[0];
        const float nx = world.normals[c0][0] + world.normals[c0 + 1][0] + world.normals[c0 + 2][0];
        const float ny = world.normals[c0][1] + world.normals[c0 + 1][1] + world.normals[c0 + 2][1];
        const float nz = world.normals[c0][2] + world.normals[c0 + 1][2] + world.normals[c0 + 2][2];
        const bool have_n = (nx * nx + ny * ny + nz * nz) > 1e-4f;
        const float ref_dot = have_n ? (g_x * nx + g_y * ny + g_z * nz) : g_y;
        if (ref_dot < 0.0f) {
          corners[1] = 2;
          corners[2] = 1;
        }
      }
      for (int k : corners) {
        const size_t c = size_t(t) * 3 + size_t(k);
        VertexKey key{};
        WorldVertex& v = key.v;
        for (int i = 0; i < 3; ++i) v.position[i] = world.positions[c][size_t(i)];
        for (int i = 0; i < 3; ++i) v.normal[i] = snorm8(world.normals[c][size_t(i)]);
        for (int i = 0; i < 2; ++i) v.uv0[i] = world.uv0[c][size_t(i)], v.uv1[i] = world.uv1[c][size_t(i)];
        if (c < world.uv2.size()) {
          v.uv2[0] = float_to_half(world.uv2[c][0]);
          v.uv2[1] = float_to_half(world.uv2[c][1]);
        } else {
          v.uv2[0] = float_to_half(world.uv1[c][0]);
          v.uv2[1] = float_to_half(world.uv1[c][1]);
        }
        for (int i = 0; i < 4; ++i) v.colour[i] = world.vertex_colours[c][size_t(i)];
        if (fade_class && t < world.instance.size())
          for (int i = 0; i < 3; ++i)
            v.centre[i] = int16_t(std::clamp(std::lround((world.instance[t][size_t(i)] - v.position[i]) * 10.0f), -32767L, 32767L));
        for (int i = 0; i < 3; ++i) {
          d.box_min[i] = std::min(d.box_min[i], v.position[i]);
          d.box_max[i] = std::max(d.box_max[i], v.position[i]);
        }
        // A tree sprite's corner sits up to |uv1.x| + |uv1.y| from its base.
        if (billboard) reach = std::max(reach, std::fabs(v.uv1[0]) + std::fabs(v.uv1[1]));
        if (!share) {
          out.indices.push_back(uint32_t(out.textured_vertices.size()));
          out.textured_vertices.push_back(v);
          continue;
        }
        const auto [it, added] = shared.try_emplace(key, uint32_t(out.textured_vertices.size()));
        if (added) out.textured_vertices.push_back(v);
        out.indices.push_back(it->second);
      }
      ++out.triangles;
    }
    for (int i = 0; i < 3; ++i) d.box_min[i] -= reach, d.box_max[i] += reach;
    first = last;
    d.index_count = uint32_t(out.indices.size()) - d.first_index;
    d.texture = mat.texture;
    d.blend = mat.blend;
    d.decal = mat.decal;
    for (int k = 0; k < 3; ++k) d.push.tint[k] = tint[size_t(k)];
    d.push.tint[3] = 1;
    d.push.params[0] = float(mat.blend);
    // world.frag's flags: 1 textured, 4 vertex-blended layers, 8 splat
    // layers, 16 decal alpha from the vertex colour's R, 32 tree sprite; a textured draw's
    // tint holds the layers' tilings.
    uint32_t flags = mat.texture != kNoWorldTexture ? 1 : 0;
    if (flags && mat.layer_b != kNoWorldTexture) {
      if (mat.layers == 1) {
        flags |= 4;
        d.texture_b = mat.layer_b;
      } else if (mat.layers == 2 && mat.splat != kNoWorldTexture) {
        flags |= 8;
        d.texture_b = mat.layer_b;
        d.splat = mat.splat;
      }
    }
    if (flags && mat.layers == 3) {
      flags |= 16;
      if (mat.splat != kNoWorldTexture) {
        d.splat = mat.splat;
        flags |= 2097152u;  // world.frag kFlagDecalNoise (H_VBLND_DECAL Noise_Sampler)
      }
    }
    if (flags && mat.layers == 4) flags |= 32;  // tree sprite: turned to the camera in world.vert
    if (mat.light_map != kNoWorldTexture) {
      d.light_map = mat.light_map;
      flags |= mat.is_emissive_map ? 8388608u : 2048u;  // kFlagEmissiveMap (slot 3) vs kFlagLightMap (slot 7)
    }
    if (mat.normal_map != kNoWorldTexture) {
      d.normal_map = mat.normal_map;
      flags |= 4096u;  // world.frag kFlagNormalMap
    }
    if (mat.ao_map != kNoWorldTexture) {
      d.ao_map = mat.ao_map;
      flags |= 8192u;  // world.frag kFlagAoMap
    }
    if (flags && mat.layer_c != kNoWorldTexture) {
      d.layer_c = mat.layer_c;
      flags |= 16384u;  // world.frag kFlagLayerC
    }
    if (mat.is_water) {
      flags |= 32768u;  // world.frag kFlagWater
      if (mat.layer_b != kNoWorldTexture) {
        d.texture_b = mat.layer_b;
        flags |= 4194304u;  // world.frag kFlagWaterNormalB (LAKE_* slot 2 normal/foam map)
      }
    }
    if (mat.anim_flag) {
      flags |= 131072u;  // world.vert kFlagAnimFlag
    }
    if (mat.is_grass) {
      flags |= 262144u;  // world.vert kFlagGrass
    }
    if (mat.spec_map != kNoWorldTexture) {
      d.spec_map = mat.spec_map;
      flags |= 524288u;  // world.frag kFlagSpecMap
    }
    if (mat.is_road) {
      flags |= 1048576u;  // world.frag kFlagRoad
    }
    if (flags & 1u) std::memcpy(d.push.tint, mat.tiling.data(), sizeof d.push.tint);
    // The distance cross-fade (world.frag): 128 a prop's LOD00, 256 its
    // LOD02, 512 a prop of one level.
    if (mat.fade >= 1 && mat.fade <= 3) flags |= 64u << mat.fade;
    d.push.params[1] = float(flags);
    // Encode integer far-ring level (0, 1, 2) plus c3.w emissive_force in [0, 0.45] so
    // world_cull.comp (params.z < 0.5) and world_vert.glsl (> 0.5 / > 1.5) stay exact.
    d.push.params[2] = float(mat.far) + std::clamp(mat.emissive_force, 0.0f, 1.0f) * 0.45f;
    // params[3] carries packed half2x16: tiling_c for H_BLND3 (kFlagLayerC),
    // or decal_noise_c4 for H_VBLND_DECAL (kFlagDecalNoise); 1.0 otherwise
    // (the obsolete 2.5x H_VBLND light gain hack is removed).
    if (flags & 16384u) {
      d.push.params[3] = pack_half2x16_float(mat.tiling_c[0], mat.tiling_c[1]);
    } else if (flags & 2097152u) {
      d.push.params[3] = pack_half2x16_float(mat.decal_noise_c4[0], mat.decal_noise_c4[1]);
    } else {
      d.push.params[3] = 1.0f;
    }
    out.draws.push_back(d);
    }
  }
  return out;
}

}  // namespace

WorldGeometry build_world(const WorldTriangles& world) {
  if (!world.material.empty()) return build_textured_world(world);
  WorldGeometry out;
  const size_t n = world.group.size();
  if (n == 0) return out;
  // One vertex per corner with the face normal as the tangent frame's
  // first axis (the shader's normal is rotate(frame, +x)); grouped by colour.
  std::vector<std::vector<uint32_t>> by_group(world.colours.size());
  for (uint32_t t = 0; t < n; ++t)
    if (world.group[t] < by_group.size()) by_group[world.group[t]].push_back(t);
  auto& vertices = out.vertices;
  auto& indices = out.indices;
  vertices.reserve(n * 3);
  indices.reserve(n * 3);
  const bool has_smooth_normals = (world.normals.size() == world.positions.size());
  for (size_t g = 0; g < by_group.size(); ++g) {
    if (by_group[g].empty()) continue;
    WorldGeometry::Draw d;
    d.first_index = uint32_t(indices.size());
    for (uint32_t t : by_group[g]) {
      const auto& a = world.positions[size_t(t) * 3];
      const auto& b = world.positions[size_t(t) * 3 + 1];
      const auto& c = world.positions[size_t(t) * 3 + 2];
      const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      float face_n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      const float flen = std::sqrt(face_n[0] * face_n[0] + face_n[1] * face_n[1] + face_n[2] * face_n[2]);
      if (!(flen > 0)) continue;
      for (float& k : face_n) k /= flen;
      if (face_n[1] < 0)
        for (float& k : face_n) k = -k;  // up-facing; the shader faces it to the eye anyway
      const std::array<float, 3>* pts[3] = {&a, &b, &c};
      for (int vi = 0; vi < 3; ++vi) {
        float nv[3] = {face_n[0], face_n[1], face_n[2]};
        if (has_smooth_normals) {
          const auto& sn = world.normals[size_t(t) * 3 + size_t(vi)];
          const float sl = std::sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]);
          if (sl > 1e-5f) nv[0] = sn[0] / sl, nv[1] = sn[1] / sl, nv[2] = sn[2] / sl;
        }
        // The rotation taking +x to the normal: (x cross n, 1 + x dot n), normalised.
        float q[4] = {0, -nv[2], nv[1], 1 + nv[0]};
        if (q[3] < 1e-4f) {
          q[0] = 0;
          q[1] = 1;
          q[2] = 0;
          q[3] = 0;
        }
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        bake::Vertex v{};
        for (int k = 0; k < 4; ++k) v.frame[size_t(k)] = int16_t(std::lround(q[k] / ql * 32767));
        v.lighting = {100, 0, 0, 0};  // open to the sky
        v.position = *pts[vi];
        indices.push_back(uint32_t(vertices.size()));
        vertices.push_back(v);
      }
      ++out.triangles;
    }
    d.index_count = uint32_t(indices.size()) - d.first_index;
    if (d.index_count == 0) continue;
    d.push.uv0[1] = d.push.uv0[3] = d.push.uv1[1] = d.push.uv1[3] = 1;
    for (int k = 0; k < 3; ++k) d.push.tint[k] = world.colours[g][size_t(k)];
    d.push.tint[3] = 1;
    d.push.params[0] = 11;  // the shader's kWorld
    d.push.params[1] = 16;  // kFlagNoSlot0
    out.draws.push_back(d);
  }
  return out;
}

bool Showroom::set_world(const WorldTriangles& world, std::string& error) {
  return set_world(build_world(world), error);
}

bool Showroom::set_world(WorldGeometry geometry, std::string& error) {
  if (arena_on() && geometry.textured && !geometry.indices.empty()) {
    r_.wait_idle();
    ++world_frame_;
    drop_pending_world();
    drop_fading_world(true);
    StagedWorld st;
    if (!stage_world(std::move(geometry), st, error, 0, 0)) return false;
    const bool ok = begin_world(std::move(st), error) && poll_world(true, nullptr);
    drop_fading_world(true);
    return ok;
  }
  r_.wait_idle();
  ++world_frame_;
  drop_pending_world();
  drop_fading_world(true);
  r_.destroy_buffer(world_vertices_);
  r_.destroy_buffer(world_indices_);
  world_draws_.clear();
  world_triangles_ = 0;
  if (geometry.indices.empty()) {
    evict_world_textures();
    release_retired();
    return true;
  }
  if (!upload_world_textures(geometry.textures, error)) return false;
  world_textured_ = geometry.textured;
  const void* vertex_data =
      geometry.textured ? static_cast<const void*>(geometry.textured_vertices.data()) : geometry.vertices.data();
  const VkDeviceSize vertex_bytes = geometry.textured ? geometry.textured_vertices.size() * sizeof(WorldVertex)
                                                      : geometry.vertices.size() * sizeof(bake::Vertex);
  if (!r_.upload_buffer(vertex_data, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, world_vertices_) ||
      !r_.upload_buffer(geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, world_indices_)) {
    error = "world buffers";
    return false;
  }
  world_draws_ = std::move(geometry.draws);
  world_triangles_ = geometry.triangles;
  evict_world_textures();
  release_retired();
  return true;
}

bool Showroom::stage_world(WorldGeometry geometry, StagedWorld& out, std::string& error, double origin_x,
                           double origin_z) {
  out = StagedWorld{};
  out.draws = std::move(geometry.draws);
  out.triangles = geometry.triangles;
  out.textured = geometry.textured;
  out.textures = std::move(geometry.textures);
  if (geometry.indices.empty()) return true;
  if (arena_on() && geometry.textured) {
    // Extract hardware-instanced tree sprite quads before index relocalization.
    for (const auto& d : out.draws) {
      const uint32_t fl = uint32_t(d.push.params[1] + 0.5f);
      if ((fl & 32u) == 0u || d.index_count < 6 || (d.index_count % 6) != 0) continue;
      for (uint32_t k = 0; k + 5 < d.index_count; k += 6) {
        const WorldVertex& v0 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 0]];
        const WorldVertex& v1 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 1]];
        const WorldVertex& v2 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 2]];
        const WorldVertex& v3 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 5]];
        if (v0.position[0] != v1.position[0] || v0.position[2] != v1.position[2]) continue;
        StagedWorld::TreeRaw tr{};
        for (int i = 0; i < 3; ++i) {
          tr.base[i] = v0.position[i];
          tr.up[i] = float(v0.normal[i]) / 127.0f;
        }
        tr.half_width = std::max(std::fabs(v0.uv1[0]), std::fabs(v1.uv1[0]));
        tr.height = std::max(v2.uv1[1], v3.uv1[1]);
        tr.uv[0] = v0.uv0[0];
        tr.uv[1] = v2.uv0[1];
        tr.uv[2] = v2.uv0[0];
        tr.uv[3] = v0.uv0[1];
        tr.rgba8 = uint32_t(v0.colour[0]) | (uint32_t(v0.colour[1]) << 8) |
                   (uint32_t(v0.colour[2]) << 16) | (uint32_t(v0.colour[3]) << 24);
        tr.texture = d.texture;
        out.trees.push_back(tr);
      }
    }
    // The resident arenas: each draw's indices made relative to its own vertices, and its content key (positions
    // made absolute, so a later gather, relative to another origin, finds the same draw).
    out.arena = true;
    for (auto& d : out.draws) {
      if (d.index_count == 0) continue;
      uint32_t lo = ~0u, hi = 0;
      for (uint32_t k = 0; k < d.index_count; ++k) {
        const uint32_t v = geometry.indices[size_t(d.first_index) + k];
        lo = std::min(lo, v), hi = std::max(hi, v);
      }
      d.vertex_begin = lo;
      d.vertex_count = hi - lo + 1;
      for (uint32_t k = 0; k < d.index_count; ++k) geometry.indices[size_t(d.first_index) + k] -= lo;
      uint64_t h = 0x9e3779b97f4a7c15ull;
      for (uint32_t t : {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c})
        h = gpuworld::hash_mix(h, t);
      h = gpuworld::hash_mix(h, uint64_t(d.blend) | uint64_t(d.decal) << 8 | uint64_t(d.cull) << 16);
      for (float f : d.push.params) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        h = gpuworld::hash_mix(h, bits);
      }
      for (float f : d.push.tint) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        h = gpuworld::hash_mix(h, bits);
      }
      h = gpuworld::hash_mix(h, uint64_t(d.index_count) << 32 | d.vertex_count);
      h = gpuworld::hash_bytes(h, &geometry.indices[d.first_index], size_t(d.index_count) * sizeof(uint32_t));
      for (uint32_t v = lo; v <= hi; ++v) {
        const WorldVertex& wv = geometry.textured_vertices[v];
        h = gpuworld::hash_mix(h, uint64_t(gpuworld::quantize_mm(double(wv.position[0]) + origin_x)));
        h = gpuworld::hash_mix(h, uint64_t(gpuworld::quantize_mm(double(wv.position[1]))));
        h = gpuworld::hash_mix(h, uint64_t(gpuworld::quantize_mm(double(wv.position[2]) + origin_z)));
        h = gpuworld::hash_bytes(h, reinterpret_cast<const unsigned char*>(&wv) + sizeof wv.position,
                                 sizeof wv - sizeof wv.position);
      }
      d.key = h;
    }
  }
  Renderer& r = r_;
  auto stage = [&](const void* data, VkDeviceSize size, std::shared_ptr<Renderer::Buffer>& buffer) {
    buffer = std::shared_ptr<Renderer::Buffer>(new Renderer::Buffer{}, [&r](Renderer::Buffer* b) {
      r.destroy_buffer(*b);
      delete b;
    });
    if (!r.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, *buffer))
      return false;
    void* mapped = nullptr;
    if (vmaMapMemory(r.allocator_, buffer->allocation, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, data, size_t(size));
    vmaUnmapMemory(r.allocator_, buffer->allocation);
    world_staging_bytes_.fetch_add(uint64_t(size), std::memory_order_relaxed);
    return true;
  };
  const bool staged_vertices =
      geometry.textured
          ? stage(geometry.textured_vertices.data(), geometry.textured_vertices.size() * sizeof(WorldVertex),
                  out.vertices)
          : stage(geometry.vertices.data(), geometry.vertices.size() * sizeof(bake::Vertex), out.vertices);
  if (!staged_vertices ||
      !stage(geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), out.indices)) {
    error = "world staging buffers";
    return false;
  }
  // The new textures' levels, packed (each from a 16-byte boundary), honouring TextureCap.
  VkDeviceSize texel_bytes = 0;
  out.texel_ranges.assign(out.textures.size(), {0, 0});
  out.texel_skips.assign(out.textures.size(), 0);
  for (size_t i = 0; i < out.textures.size(); ++i) {
    const auto& t = out.textures[i];
    if (!t || t->texture.levels.empty()) continue;
    VkDeviceSize bytes = 0;
    const uint32_t levels = texture_chain(t->texture, bytes);
    if (!levels) continue;  // uploaded apart, which reports it
    const uint32_t skip = modern::texture_levels_to_skip(t->texture.levels[0].width, t->texture.levels[0].height,
                                                         levels, texture_cap_);
    VkDeviceSize kept_bytes = 0;
    for (uint32_t l = skip; l < levels; ++l) kept_bytes += t->texture.levels[l].data.size();
    out.texel_ranges[i] = {texel_bytes, levels};
    out.texel_skips[i] = skip;
    texel_bytes += (kept_bytes + 15) / 16 * 16;
  }
  if (texel_bytes) {
    std::vector<uint8_t> packed(size_t(texel_bytes), 0);
    for (size_t i = 0; i < out.textures.size(); ++i) {
      if (!out.texel_ranges[i].second) continue;
      size_t at = size_t(out.texel_ranges[i].first);
      for (uint32_t l = out.texel_skips[i]; l < out.texel_ranges[i].second; ++l) {
        const auto& data = out.textures[i]->texture.levels[l].data;
        std::memcpy(packed.data() + at, data.data(), data.size());
        at += data.size();
      }
    }
    if (!stage(packed.data(), texel_bytes, out.texels)) {
      out.texels.reset();  // every texture uploads apart instead
      out.texel_ranges.assign(out.textures.size(), {0, 0});
      out.texel_skips.assign(out.textures.size(), 0);
    }
  }
  // Their images and views too (thread-safe device calls; on the render
  // thread they cost a fraction of a millisecond each, tens per gather).
  if (out.texels && out.vertices && out.indices) {
    out.images = std::shared_ptr<std::vector<Renderer::Image>>(
        new std::vector<Renderer::Image>(out.textures.size()), [&r](std::vector<Renderer::Image>* v) {
          for (auto& i : *v) r.destroy_image(i);
          delete v;
        });
    for (size_t i = 0; i < out.textures.size(); ++i) {
      const auto& t = out.textures[i];
      if (!t || !out.texel_ranges[i].second) continue;
      {
        std::lock_guard<std::mutex> lock(world_textures_mutex_);
        if (world_textures_.count(t->record)) continue;
      }
      std::string why;
      if (!create_texture_image(t->texture, t->srgb, out.texel_ranges[i].second, (*out.images)[i], why,
                                out.texel_skips[i]))
        r.destroy_image((*out.images)[i]);  // begin_world tries again and reports it
    }
  }
  return true;
}

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

void Showroom::drop_pending_world() {
  if (!pending_world_) return;
  auto& p = *pending_world_;
  if (p.fence) {
    vkWaitForFences(r_.device_, 1, &p.fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(r_.device_, p.fence, nullptr);
  }
  if (p.fence)  // submitted and waited: the copies ran
    for (const auto& d : p.staged.draws)
      if (d.resident && arena_on()) gpu_world_->resident().mark_valid(d.key);
  world_release_draws(p.staged.draws);
  if (p.cmd) vkFreeCommandBuffers(r_.device_, r_.pool_, 1, &p.cmd);
  // Its copy is done (the fence): the staging buffers, the worker's
  // images not taken and buffers never swapped in go on the reaper.
  auto staged = std::make_shared<StagedWorld>(std::move(p.staged));
  reap([this, staged, v = p.vertices, i = p.indices]() mutable {
    r_.destroy_buffer(v);
    r_.destroy_buffer(i);
    staged.reset();
  });
  pending_world_.reset();
}

void Showroom::reap(std::function<void()> f) {
  if (!reaper_.joinable()) {
    reaper_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(reap_mutex_);
      for (;;) {
        reap_cv_.wait(lock, [this] { return reap_stop_ || !reap_queue_.empty(); });
        if (reap_queue_.empty()) return;  // stopping, and drained
        auto job = std::move(reap_queue_.front());
        reap_queue_.pop_front();
        lock.unlock();
        job();
        lock.lock();
      }
    });
  }
  {
    std::lock_guard<std::mutex> lock(reap_mutex_);
    reap_queue_.push_back(std::move(f));
  }
  reap_cv_.notify_one();
}

void Showroom::retire(std::function<void()> f) { retired_.push_back({r_.frames_submitted(), std::move(f)}); }

void Showroom::release_retired() {
  const uint64_t done = r_.frames_done();
  for (size_t k = 0; k < retired_.size();) {
    if (retired_[k].first <= done) {
      reap(std::move(retired_[k].second));
      retired_.erase(retired_.begin() + ptrdiff_t(k));
    } else {
      ++k;
    }
  }
  for (size_t k = 0; k < retired_sets_.size();) {
    if (retired_sets_[k].first <= done) {
      free_world_sets_.insert(free_world_sets_.end(), retired_sets_[k].second.begin(), retired_sets_[k].second.end());
      retired_sets_.erase(retired_sets_.begin() + ptrdiff_t(k));
    } else {
      ++k;
    }
  }
}

void Showroom::stop_reaper() {
  // The GPU is idle (the caller waited): what is retired can go now.
  for (auto& [tag, f] : retired_) f();
  retired_.clear();
  retired_sets_.clear();
  free_world_sets_.clear();
  if (!reaper_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(reap_mutex_);
    reap_stop_ = true;
  }
  reap_cv_.notify_one();
  reaper_.join();
}

bool Showroom::begin_world(StagedWorld staged, std::string& error) {
  // One copy in flight at a time: a newer world replaces a pending one.
  drop_pending_world();
  // Textures staged by the worker are copied in the scenery's fenced
  // upload below; the rest (and every texture of a world with no buffers)
  // are uploaded here, one wait each.
  const bool fenced = staged.vertices && staged.indices && staged.texels;
  std::vector<std::shared_ptr<const WorldTextureData>> apart;
  for (size_t i = 0; i < staged.textures.size(); ++i)
    if (!fenced || i >= staged.texel_ranges.size() || !staged.texel_ranges[i].second)
      apart.push_back(staged.textures[i]);
  if (!upload_world_textures(apart, error)) return false;
  auto p = std::make_unique<PendingWorld>();
  p->staged = std::move(staged);
  if (p->staged.vertices && p->staged.indices) {
    const VkDeviceSize vb = p->staged.vertices->size, ib = p->staged.indices->size;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = r_.pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const bool arena = p->staged.arena && arena_on();
    std::vector<WorldGpu::Copy> arena_vertex_copies, arena_index_copies;
    if (arena && !place_world_arena(p->staged, arena_vertex_copies, arena_index_copies, error)) {
      // Long-distance teleports (e.g. New Career / Fast Travel) can temporarily exceed the
      // resident arena while old cells are still live. Flush old/fading draws and retry once.
      r_.wait_idle();
      drop_fading_world(true);
      for (auto& d : world_draws_) {
        if (d.resident) gpu_world_->resident().release(d.key, 0);
      }
      world_draws_.clear();
      arena_vertex_copies.clear();
      arena_index_copies.clear();
      error.clear();
      if (!place_world_arena(p->staged, arena_vertex_copies, arena_index_copies, error)) {
        pending_world_ = std::move(p);
        drop_pending_world();
        return false;
      }
    }
    const VkBufferUsageFlags world_rt_flags =
        r_.rt_supported()
            ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
            : 0u;
    if ((!arena && (!r_.create_buffer(vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | world_rt_flags,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p->vertices) ||
                    !r_.create_buffer(ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | world_rt_flags,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p->indices))) ||
        vkAllocateCommandBuffers(r_.device_, &alloc, &p->cmd) != VK_SUCCESS ||
        vkCreateFence(r_.device_, &fence, nullptr, &p->fence) != VK_SUCCESS) {
      pending_world_ = std::move(p);
      drop_pending_world();
      error = "world buffers";
      return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(p->cmd, &begin);
    if (arena) {
      // Only the draws the arenas do not hold yet.
      gpu_world_->record_copies(p->cmd, p->staged.vertices->buffer, arena_vertex_copies, p->staged.indices->buffer,
                                arena_index_copies);
    } else {
      const VkBufferCopy v{0, 0, vb}, i{0, 0, ib};
      vkCmdCopyBuffer(p->cmd, p->staged.vertices->buffer, p->vertices.buffer, 1, &v);
      vkCmdCopyBuffer(p->cmd, p->staged.indices->buffer, p->indices.buffer, 1, &i);
    }
    if (fenced) {
      const auto& st = p->staged;
      for (size_t k = 0; k < st.textures.size() && k < st.texel_ranges.size(); ++k) {
        const auto& t = st.textures[k];
        if (!t || !st.texel_ranges[k].second) continue;
        {
          std::lock_guard<std::mutex> lock(world_textures_mutex_);
          if (world_textures_.count(t->record)) continue;
        }
        std::string why;
        Renderer::Image* made = st.images && k < st.images->size() ? &(*st.images)[k] : nullptr;
        const uint32_t skip = k < st.texel_skips.size() ? st.texel_skips[k] : 0u;
        if (!begin_world_texture(*t, st.texels->buffer, st.texel_ranges[k].first, st.texel_ranges[k].second, skip,
                                 p->cmd, made, why))
          notes_.push_back("world texture " + t->texture.name + ": " + why);  // drawn flat
      }
    }
    vkEndCommandBuffer(p->cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &p->cmd;
    VkResult submitted;
    {
      std::lock_guard<std::mutex> qlock(r_.queue_mutex_);
      submitted = vkQueueSubmit(r_.queue_, 1, &submit, p->fence);
    }
    if (submitted != VK_SUCCESS) {
      vkDestroyFence(r_.device_, p->fence, nullptr);
      p->fence = VK_NULL_HANDLE;
      pending_world_ = std::move(p);
      drop_pending_world();
      error = "world upload submit";
      return false;
    }
  }
  pending_world_ = std::move(p);
  evict_world_textures();
  return true;
}

bool Showroom::poll_world(bool wait, bool* swapped) {
  if (swapped) *swapped = false;
  release_retired();
  if (!pending_world_) return true;
  auto& p = *pending_world_;
  if (p.fence) {
    if (!wait && vkGetFenceStatus(r_.device_, p.fence) != VK_SUCCESS) return true;
    vkWaitForFences(r_.device_, 1, &p.fence, VK_TRUE, UINT64_MAX);
  }
  // The scenery in use may still be read by the frame in flight: it goes
  // once that frame is done, off this thread (no device wait here). With a
  // swap fade it is kept and drawn dithered under the new one first (ours:
  // a regather's changes cross-fade rather than pop).
  drop_fading_world(false);
  if (p.staged.arena && arena_on()) {
    // Resident draws: the new scenery's references were taken in begin_world. What the old scenery drew that the
    // new one does not is kept, dithered out; the rest of its references go (the arenas keep the content cached).
    const uint64_t tag = r_.frames_submitted();
    std::unordered_set<uint64_t> new_keys;
    for (const auto& d : p.staged.draws)
      if (d.resident) {
        new_keys.insert(d.key);
        gpu_world_->resident().mark_valid(d.key);  // the copy is done (the fence above)
      }
    std::unordered_set<uint64_t> old_keys;
    for (const auto& d : world_draws_)
      if (d.resident) old_keys.insert(d.key);
    const bool fade = swap_fade_seconds_ > 0 && world_textured_ && !world_draws_.empty();
    fading_ = FadingWorld{};
    fading_.arena = true;
    fading_.origin[0] = world_origin_[0];
    fading_.origin[1] = world_origin_[1];
    for (auto& d : world_draws_) {
      if (!d.resident) continue;
      if (fade && d.cull && !new_keys.count(d.key)) {
        d.fade_in = false;
        fading_.draws.push_back(d);  // keeps its reference until the fade ends
      } else {
        gpu_world_->resident().release(d.key, tag);
      }
    }
    world_draws_ = std::move(p.staged.draws);
    for (auto& d : world_draws_) d.fade_in = fade && d.resident && d.cull && !old_keys.count(d.key);
    if (!fading_.draws.empty()) {
      fading_on_ = true;
      fade_start_ = std::chrono::steady_clock::now();
    }
    world_triangles_ = p.staged.triangles;
    world_textured_ = p.staged.textured;
    {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      for (const auto& d : world_draws_) {
        if (d.texture != kNoWorldTexture) world_lru_.touch(d.texture, world_frame_);
        if (d.texture_b != kNoWorldTexture) world_lru_.touch(d.texture_b, world_frame_);
        if (d.splat != kNoWorldTexture) world_lru_.touch(d.splat, world_frame_);
        if (d.light_map != kNoWorldTexture) world_lru_.touch(d.light_map, world_frame_);
        if (d.normal_map != kNoWorldTexture) world_lru_.touch(d.normal_map, world_frame_);
        if (d.ao_map != kNoWorldTexture) world_lru_.touch(d.ao_map, world_frame_);
        if (d.layer_c != kNoWorldTexture) world_lru_.touch(d.layer_c, world_frame_);
      }
    }
    const bool origin_changed = (world_origin_[0] != p.staged.origin_x || world_origin_[1] != p.staged.origin_z);
    world_origin_[0] = p.staged.origin_x;
    world_origin_[1] = p.staged.origin_z;
    world_trees_ = std::move(p.staged.trees);
    if (origin_changed) {
      gpu_world_->invalidate_hiz();
      have_prev_vp_ = false;
    }
    std::string table_error;
    if (!build_gpu_table(table_error)) notes_.push_back("world draw table: " + table_error);
    drop_pending_world();
    evict_world_textures();
    release_retired();
    if (swapped) *swapped = true;
    return true;
  }
  if (swap_fade_seconds_ > 0 && world_textured_ && p.staged.textured && world_vertices_.buffer && p.vertices.buffer) {
    fading_.vertices = world_vertices_;
    fading_.indices = world_indices_;
    fading_.draws = std::move(world_draws_);
    fading_.origin[0] = world_origin_[0];
    fading_.origin[1] = world_origin_[1];
    fading_on_ = true;
    fade_start_ = std::chrono::steady_clock::now();
  } else {
    retire([this, v = world_vertices_, i = world_indices_]() mutable {
      r_.destroy_buffer(v);
      r_.destroy_buffer(i);
    });
  }
  world_vertices_ = p.vertices;
  world_indices_ = p.indices;
  p.vertices = {};
  p.indices = {};
  if (world_vertices_.buffer) {
    world_draws_ = std::move(p.staged.draws);
    world_triangles_ = p.staged.triangles;
    world_textured_ = p.staged.textured;
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    for (const auto& d : world_draws_) {
      if (d.texture != kNoWorldTexture) world_lru_.touch(d.texture, world_frame_);
      if (d.texture_b != kNoWorldTexture) world_lru_.touch(d.texture_b, world_frame_);
      if (d.splat != kNoWorldTexture) world_lru_.touch(d.splat, world_frame_);
      if (d.light_map != kNoWorldTexture) world_lru_.touch(d.light_map, world_frame_);
      if (d.normal_map != kNoWorldTexture) world_lru_.touch(d.normal_map, world_frame_);
      if (d.ao_map != kNoWorldTexture) world_lru_.touch(d.ao_map, world_frame_);
      if (d.layer_c != kNoWorldTexture) world_lru_.touch(d.layer_c, world_frame_);
    }
  } else {
    world_draws_.clear();
    world_triangles_ = 0;
  }
  world_origin_[0] = p.staged.origin_x;
  world_origin_[1] = p.staged.origin_z;
  drop_pending_world();
  evict_world_textures();
  release_retired();
  if (swapped) *swapped = true;
  return true;
}

bool Showroom::set_world(StagedWorld staged, std::string& error) {
  return begin_world(std::move(staged), error) && poll_world(true, nullptr);
}

void Showroom::drop_fading_world(bool now) {
  if (!fading_on_) return;
  fading_on_ = false;
  if (fading_.arena) {
    world_release_draws(fading_.draws);
    fading_.draws.clear();
    fading_.arena = false;
    if (!now) evict_world_textures();
    return;
  }
  fading_.draws.clear();
  if (now) {
    r_.destroy_buffer(fading_.vertices);
    r_.destroy_buffer(fading_.indices);
  } else {
    retire([this, v = fading_.vertices, i = fading_.indices]() mutable {
      r_.destroy_buffer(v);
      r_.destroy_buffer(i);
    });
    evict_world_textures();
  }
  fading_.vertices = {};
  fading_.indices = {};
}

void Showroom::set_world_transform(const float transform[16]) {
  std::memcpy(world_transform_, transform, sizeof world_transform_);
  for (auto& d : world_draws_) std::memcpy(d.push.model, transform, sizeof d.push.model);
}

void Showroom::set_world_light(const float sun[3], const float up[3], float exposure) {
  for (int k = 0; k < 3; ++k) world_sun_[k] = sun[k], world_up_[k] = up[k];
  world_up_[3] = exposure;
  world_lit_ = true;
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

void Showroom::record_world(VkCommandBuffer cmd) {
  record_world_opaque(cmd);
  if (world_textured_ && sky_ready_) {
    record_sky_draw(cmd);
    r_.mark(cmd, "sky");
  }
  record_world_blended(cmd);
}

void Showroom::record_world_opaque(VkCommandBuffer cmd) {
  current_world_swap_ = 2.0f;
  if (world_draws_.empty() || material_sets_.empty()) return;
  world_bind_buffers(cmd, false);
  if (world_textured_) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
    world_stats_ = {};
    float swap = 2.0f;
    if (fading_on_) {
      const double t =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - fade_start_).count() / swap_fade_seconds_;
      if (t >= 1.0) drop_fading_world(false);
      else swap = float(std::max(t, 0.0));
    }
    current_world_swap_ = swap;
    if (arena_on() && gpu_world_->table_rows() > 0) {
      // The opaque and cut-out draws, culled on the GPU into indirect batches.
      WorldGpu::DrawParams dp;
      dp.frame_set = frame_set_;
      std::memcpy(dp.model, world_transform_, sizeof dp.model);
      for (int k = 0; k < 3; ++k) dp.sun[k] = world_sun_[k];
      for (int k = 0; k < 4; ++k) dp.up[k] = world_up_[k];
      dp.swap = swap;
      dp.bias_sign = r_.reverse_z() ? 1.0f : -1.0f;
      dp.decal_bias_constant = kDecalBiasConstant;
      dp.decal_bias_slope = kDecalBiasSlope;
      dp.depth_prepass = r_.z_prepass();
      gpu_world_->record_draw(cmd, dp);
      const size_t tested = gpu_world_->last_tested(), drawn = gpu_world_->last_drawn();
      const size_t trees_drawn = gpu_world_->last_trees_drawn();
      world_stats_.drawn += drawn + (trees_drawn > 0 ? 1 : 0);
      world_stats_.culled += tested > drawn ? tested - drawn : 0;
      world_stats_.indices += trees_drawn * 6;
      world_stats_.white += gpu_table_white_;
      world_stats_.layer_fallback += gpu_table_layer_fallback_;
      r_.mark(cmd, "world gpu");
    }
    record_world_textured(cmd, world_draws_, world_transform_, swap, false, 1);
    if (fading_on_) {
      const float dx = float(fading_.origin[0] - world_origin_[0]), dz = float(fading_.origin[1] - world_origin_[1]);
      float m[16];
      std::memcpy(m, world_transform_, sizeof m);
      for (int r = 0; r < 4; ++r) m[12 + r] += world_transform_[r] * dx + world_transform_[8 + r] * dz;
      world_bind_buffers(cmd, true);
      record_world_textured(cmd, fading_.draws, m, 3.0f + swap, true, 1);
    }
    return;
  }
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &material_sets_[0], 0,
                          nullptr);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_);
  r_.apply_vrs(cmd);
  for (const auto& d : world_draws_) {
    ShowroomPush push = d.push;
    std::memcpy(push.model, world_transform_, sizeof push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
  }
}

void Showroom::record_world_blended(VkCommandBuffer cmd) {
  if (!world_textured_ || world_draws_.empty() || material_sets_.empty()) return;
  const float swap = current_world_swap_;
  world_bind_buffers(cmd, false);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  record_world_textured(cmd, world_draws_, world_transform_, swap, false, 2);
  if (fading_on_) {
    const float dx = float(fading_.origin[0] - world_origin_[0]), dz = float(fading_.origin[1] - world_origin_[1]);
    float m[16];
    std::memcpy(m, world_transform_, sizeof m);
    for (int r = 0; r < 4; ++r) m[12 + r] += world_transform_[r] * dx + world_transform_[8 + r] * dz;
    world_bind_buffers(cmd, true);
    record_world_textured(cmd, fading_.draws, m, 3.0f + swap, true, 2);
  }
  if (overlay_count_ >= 3 && overlay_vertices_.buffer && world_alpha_ && world_white_set_) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &overlay_vertices_.buffer, &off);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_alpha_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &world_white_set_, 0,
                            nullptr);
    const float sign = r_.reverse_z() ? 1.0f : -1.0f;
    vkCmdSetDepthBias(cmd, sign * kDecalBiasConstant, 0.0f, sign * kDecalBiasSlope);
    ShowroomPush push{};
    std::memcpy(push.model, world_transform_, sizeof push.model);
    std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
    std::memcpy(push.uv1, world_up_, sizeof push.uv1);
    push.tint[0] = push.tint[1] = push.tint[2] = push.tint[3] = 1.0f;
    push.params[0] = 2.0f;     // kAlpha
    push.params[1] = 1024.0f;  // kFlagUnlitVertexColor
    push.params[2] = 2.0f;     // no swap dither
    push.params[3] = 1.0f;
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDraw(cmd, overlay_count_, 1, 0, 0);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  }
}

// The camera's eye in the scenery's frame (the transform is a rotation and
// a translation).
std::array<float, 3> Showroom::eye_in_scenery(const float transform[16]) const {
  const float e[3] = {frame_.eye[0] - transform[12], frame_.eye[1] - transform[13], frame_.eye[2] - transform[14]};
  std::array<float, 3> out{};
  for (int c = 0; c < 3; ++c) out[size_t(c)] = transform[c * 4 + 0] * e[0] + transform[c * 4 + 1] * e[1] + transform[c * 4 + 2] * e[2];
  return out;
}

// A cell of prop LOD00 copies wholly past the LOD band from the eye: every
// instance in it would be hidden (world.vert), so it is not drawn at all.
bool Showroom::near_lod_beyond(const WorldGeometry::Draw& d, const std::array<float, 3>& eye) const {
  if ((uint32_t(d.push.params[1] + 0.5f) & 128u) == 0 || world_fade_[1] <= 0) return false;
  float d2 = 0;
  for (int i = 0; i < 3; ++i) {
    const float v = std::max({d.box_min[i] - eye[size_t(i)], 0.0f, eye[size_t(i)] - d.box_max[i]});
    d2 += v * v;
  }
  const float reach = world_fade_[0] + world_fade_[1] + kPropReach;
  return d2 > reach * reach;
}

void Showroom::record_world_textured(VkCommandBuffer cmd, const std::vector<WorldGeometry::Draw>& draws,
                                     const float transform[16], float swap, bool outgoing, int pass_mode) {
  {
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    // The benchmark's split (Renderer::mark): the sky, then each blend group.
    static const char* const kGroup[] = {"world opaque", "world cut-outs", "world blended"};
    int group = outgoing ? 99 : -1;  // the outgoing scenery adds no marks
    // The view's side planes in the scenery's frame (clip = view_projection
    // x the scenery's transform, column-major): w + x, w - x, w + y, w - y.
    // A box wholly outside one of them is not drawn. The near and far
    // planes are left out (the far ring's depth push).
    float clip[16];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += frame_.view_projection[k * 4 + r] * transform[c * 4 + k];
        clip[c * 4 + r] = s;
      }
    float planes[4][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], w = clip[c * 4 + 3];
      planes[0][c] = w + x, planes[1][c] = w - x, planes[2][c] = w + y, planes[3][c] = w - y;
    }
    auto outside = [&](const WorldGeometry::Draw& d) {
      for (const auto& p : planes) {
        // The box's corner furthest along the plane's normal.
        const float x = p[0] >= 0 ? d.box_max[0] : d.box_min[0];
        const float y = p[1] >= 0 ? d.box_max[1] : d.box_min[1];
        const float z = p[2] >= 0 ? d.box_max[2] : d.box_min[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return true;
      }
      return false;
    };
    int bias = -1;  // the depth bias set: -1 none yet, else the decal flag
    const auto eye = eye_in_scenery(transform);
    for (const auto& d : draws) {
      if (pass_mode == 1 && d.blend >= 2) continue;
      if (pass_mode == 2 && d.blend < 2) continue;
      if (!outgoing && d.gpu) continue;  // drawn by the culling pass
      if (outgoing && !d.cull) continue;  // the sky: the incoming one's is drawn whole
      if (d.cull && ((world_cull_ && outside(d)) || near_lod_beyond(d, eye))) {
        ++world_stats_.culled;
        continue;
      }
      ++world_stats_.drawn;
      world_stats_.indices += d.index_count;
      const uint32_t draw_fl = uint32_t(d.push.params[1] + 0.5f);
      const bool sky = draw_fl == 2;
      if (sky && sky_ready_) continue;
      if (group < 0 && !sky) {
        if (!sky_ready_) r_.mark(cmd, "sky");
        group = d.blend;
      }
      if (group >= 0 && group < 99 && d.blend != group) {
        r_.mark(cmd, kGroup[std::min(group, 2)]);
        group = d.blend;
      }
      // Additive (3) shares the blended pipeline: its fragments carry alpha 0.
      const bool needs_dither =
          (swap < 1.5f || swap > 2.5f) || ((draw_fl & (128u | 256u | 512u)) != 0u) || sky;
      const VkPipeline op_pipe = (needs_dither && world_opaque_dither_)
                                     ? world_opaque_dither_
                                     : ((r_.z_prepass() && world_opaque_after_z_) ? world_opaque_after_z_ : world_opaque_);
      const VkPipeline pipeline =
          d.blend >= 2 ? (((draw_fl & 32768u) != 0u && world_water_) ? world_water_ : world_alpha_)
                       : d.blend == 1 ? world_coverage_
                                      : op_pipe;
      if (pipeline != bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
        if (d.blend < 2) r_.apply_vrs(cmd);
        bias = -1;
      }
      // Decals and road paint: pulled towards the eye by a few float ulps
      // and the slope (ours; the sign follows the depth direction).
      if (int(d.decal) != bias) {
        bias = d.decal;
        const float sign = r_.reverse_z() ? 1.0f : -1.0f;
        vkCmdSetDepthBias(cmd, d.decal ? sign * kDecalBiasConstant : 0.0f, 0.0f, d.decal ? sign * kDecalBiasSlope : 0.0f);
      }
      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      if (set) {
        std::lock_guard<std::mutex> lock(world_textures_mutex_);
        world_lru_.touch(d.texture, world_frame_);
      }
      ShowroomPush push = d.push;
      const bool has_extra = d.texture_b != kNoWorldTexture || d.splat != kNoWorldTexture ||
                             d.light_map != kNoWorldTexture || d.normal_map != kNoWorldTexture ||
                             d.ao_map != kNoWorldTexture || d.layer_c != kNoWorldTexture ||
                             d.spec_map != kNoWorldTexture;
      if ((set || d.texture == kNoWorldTexture) && has_extra) {
        VkDescriptorSet layered = world_layer_set(
            {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map});
        if (layered) {
          set = layered;
          std::lock_guard<std::mutex> lock(world_textures_mutex_);
          if (d.texture_b != kNoWorldTexture) world_lru_.touch(d.texture_b, world_frame_);
          if (d.splat != kNoWorldTexture) world_lru_.touch(d.splat, world_frame_);
          if (d.light_map != kNoWorldTexture) world_lru_.touch(d.light_map, world_frame_);
          if (d.normal_map != kNoWorldTexture) world_lru_.touch(d.normal_map, world_frame_);
          if (d.ao_map != kNoWorldTexture) world_lru_.touch(d.ao_map, world_frame_);
          if (d.layer_c != kNoWorldTexture) world_lru_.touch(d.layer_c, world_frame_);
          if (d.spec_map != kNoWorldTexture) world_lru_.touch(d.spec_map, world_frame_);
        } else if (d.texture_b != kNoWorldTexture) {
          push.params[1] = 1, ++world_stats_.layer_fallback;  // a layer did not upload: slot 0 alone
        }
      }
      if (!set) {
        if (d.texture != kNoWorldTexture) ++world_stats_.white;
        set = world_white_set_;
        if (push.params[1] == 1) push.params[1] = 0;  // the texture did not upload: flat
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &(bound_set = set),
                                0, nullptr);
      // Cut-outs without MSAA: world.frag's alpha test (flag 64).
      if (d.blend == 1 && r_.samples_ == VK_SAMPLE_COUNT_1_BIT)
        push.params[1] = float(uint32_t(push.params[1] + 0.5f) | 64u);
      world_draw_model(d, transform, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = swap;  // world.frag's swap dither
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
    if (group >= 0 && group < 99) r_.mark(cmd, kGroup[std::min(group, 2)]);
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

void Showroom::set_tod(const world::TodState& tod, int moon_phase, float real_time_sec, float eye_game_y) {
  tod_active_ = true;
  tod_state_ = tod;
  tod_moon_phase_ = std::clamp(moon_phase, 0, 4);
  tod_real_time_ = real_time_sec;
  tod_eye_game_y_ = eye_game_y;
}

void Showroom::apply_tod_to_frame() {
  if (!tod_active_) {
    frame_.sun_color[3] = -1.0f;
    frame_.car_sun_color[3] = -1.0f;
    return;
  }
  const auto& t = tod_state_;
  // Map ShadowIntensity [-1..+1] to shadow weight [0.15..1.0] (softening shadows under moonlight).
  const float shadow_w = std::clamp(0.5f * (t.shadow_intensity + 1.0f), 0.12f, 1.0f);
  frame_.sun_color[0] = t.sun_color[0];
  frame_.sun_color[1] = t.sun_color[1];
  frame_.sun_color[2] = t.sun_color[2];
  frame_.sun_color[3] = shadow_w;

  frame_.car_sun_color[0] = t.car_sun_color[0];
  frame_.car_sun_color[1] = t.car_sun_color[1];
  frame_.car_sun_color[2] = t.car_sun_color[2];
  frame_.car_sun_color[3] = t.ibl_direct_car;

  frame_.sky_color[0] = t.hl_top[0];
  frame_.sky_color[1] = t.hl_top[1];
  frame_.sky_color[2] = t.hl_top[2];
  frame_.sky_color[3] = t.switch_on_lights;

  frame_.ground_color[0] = t.hl_bottom[0];
  frame_.ground_color[1] = t.hl_bottom[1];
  frame_.ground_color[2] = t.hl_bottom[2];
  frame_.ground_color[3] = t.day_night_post;

  frame_.fog_color[0] = t.fog_color[0];
  frame_.fog_color[1] = t.fog_color[1];
  frame_.fog_color[2] = t.fog_color[2];
  frame_.fog_color[3] = t.fog_start;

  frame_.fog_scatter[0] = t.fog_sun_scatter_color[0];
  frame_.fog_scatter[1] = t.fog_sun_scatter_color[1];
  frame_.fog_scatter[2] = t.fog_sun_scatter_color[2];
  frame_.fog_scatter[3] = t.fog_sun_scatter_power;

  // Convert TimeOfDayA FogDensity (~0.03 day .. 0.08 night) to exp2 distance rate.
  frame_.fog_params[0] = std::max(t.fog_density, 0.002f) * 0.015f;
  frame_.fog_params[1] = t.fog_height_fade_start;
  frame_.fog_params[2] = t.fog_height_fade_end;
  frame_.fog_params[3] = t.fog_height_fade_amount;

  frame_.car_light_top[0] = t.car_hl_top[0];
  frame_.car_light_top[1] = t.car_hl_top[1];
  frame_.car_light_top[2] = t.car_hl_top[2];
  frame_.car_light_top[3] = t.ibl_ambient_scale;

  frame_.car_light_bot[0] = t.car_hl_bottom[0];
  frame_.car_light_bot[1] = t.car_hl_bottom[1];
  frame_.car_light_bot[2] = t.car_hl_bottom[2];
  frame_.car_light_bot[3] = tod_eye_game_y_;

  frame_.ambient_color[0] = t.ambient_color[0];
  frame_.ambient_color[1] = t.ambient_color[1];
  frame_.ambient_color[2] = t.ambient_color[2];
  frame_.ambient_color[3] = t.ibl_ambient_scale;
}

bool Showroom::upload_sky(const world::RealTimeSkyAssets& assets, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (sky_ready_) return true;

  auto upload_rgba = [&](const fh1::RgbaImage& img, GpuTexture& out, bool fallback_black) -> bool {
    bake::Texture tex;
    tex.format = bake::TextureFormat::kRgba8;
    if (img.width > 0 && img.height > 0 && img.rgba.size() == size_t(img.width) * img.height * 4) {
      tex.levels.push_back({img.width, img.height, img.rgba});
    } else {
      const uint8_t v = fallback_black ? 0 : 255;
      tex.levels.push_back({1, 1, {v, v, v, v}});
    }
    return upload_texture(&tex, false, out, error);
  };

  if (!upload_rgba(assets.sun, sky_sun_, true)) return false;
  for (int i = 0; i < 5; ++i)
    if (!upload_rgba(assets.moons[size_t(i)], sky_moons_[i], true)) return false;
  if (!upload_rgba(assets.dither, sky_dither_, false)) return false;
  if (!upload_rgba(assets.far_cloud_front, sky_far_f_, true)) return false;
  if (!upload_rgba(assets.far_cloud_back, sky_far_b_, true)) return false;
  if (!upload_rgba(assets.far_cloud_mask, sky_far_mask_, false)) return false;
  if (!upload_rgba(assets.close_alpha_front, sky_close_ad_f_, true)) return false;
  if (!upload_rgba(assets.close_alpha_back, sky_close_ad_b_, true)) return false;
  if (!upload_rgba(assets.close_ambient_front, sky_close_am_f_, false)) return false;
  if (!upload_rgba(assets.close_ambient_back, sky_close_am_b_, false)) return false;
  if (!upload_rgba(assets.close_tb_front, sky_close_tb_f_, true)) return false;
  if (!upload_rgba(assets.close_tb_back, sky_close_tb_b_, true)) return false;
  sky_has_textures_ = assets.loaded();

  if (!assets.stars.empty()) {
    std::vector<float> star_data;
    star_data.reserve(assets.stars.size() * 4);
    for (const auto& s : assets.stars) {
      star_data.push_back(s.x);
      star_data.push_back(s.y);
      star_data.push_back(s.z);
      star_data.push_back(s.brightness);
    }
    if (!r_.upload_buffer(star_data.data(), star_data.size() * sizeof(float),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, star_vertices_)) {
      error = "starfield vertex buffer";
      return false;
    }
    star_count_ = uint32_t(assets.stars.size());
  }

  for (int i = 0; i < 2; ++i) {
    if (!r_.create_buffer(sizeof(SkyUniforms),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sky_ubo_[i])) {
      error = "sky uniform buffer";
      return false;
    }
  }

  // Sky descriptor set layout: binding 0 UBO, bindings 1..11 combined image samplers.
  VkDescriptorSetLayoutBinding bindings[12]{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  for (uint32_t b = 1; b < 12; ++b)
    bindings[b] = {b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_ci.bindingCount = 12;
  set_ci.pBindings = bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set_ci, nullptr, &sky_set_layout_), "sky set layout");

  VkPipelineLayoutCreateInfo pl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_ci.setLayoutCount = 1;
  pl_ci.pSetLayouts = &sky_set_layout_;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &pl_ci, nullptr, &sky_pipe_layout_), "sky pipeline layout");

  // Stars pipeline layout: push constants (mat4 + vec4 = 80 bytes).
  VkPushConstantRange star_pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};
  VkPipelineLayoutCreateInfo spl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  spl_ci.pushConstantRangeCount = 1;
  spl_ci.pPushConstantRanges = &star_pc;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &spl_ci, nullptr, &stars_pipe_layout_), "stars pipeline layout");

  // Descriptor pool & 12 sets (2 modes x 6 discs: 0=sun, 1..5=moon0..4).
  const VkDescriptorPoolSize psizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * 11}};
  VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_ci.maxSets = 12;
  pool_ci.poolSizeCount = 2;
  pool_ci.pPoolSizes = psizes;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &pool_ci, nullptr, &sky_pool_), "sky descriptor pool");

  VkDescriptorSetLayout set_layouts[12];
  for (int i = 0; i < 12; ++i) set_layouts[i] = sky_set_layout_;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = sky_pool_;
  alloc.descriptorSetCount = 12;
  alloc.pSetLayouts = set_layouts;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, sky_sets_), "sky descriptor sets");

  for (int mode = 0; mode < 2; ++mode) {
    for (int disc = 0; disc < 6; ++disc) {
      const int idx = mode * 6 + disc;
      VkDescriptorBufferInfo buf{sky_ubo_[mode].buffer, 0, sizeof(SkyUniforms)};
      const VkImageView views[11] = {
          disc == 0 ? sky_sun_.view : sky_moons_[disc - 1].view,
          sky_dither_.view,
          sky_far_f_.view,
          sky_far_b_.view,
          sky_far_mask_.view,
          sky_close_ad_f_.view,
          sky_close_ad_b_.view,
          sky_close_am_f_.view,
          sky_close_am_b_.view,
          sky_close_tb_f_.view,
          sky_close_tb_b_.view,
      };
      VkDescriptorImageInfo imgs[11];
      VkWriteDescriptorSet writes[12]{};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[0].dstSet = sky_sets_[idx];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      writes[0].pBufferInfo = &buf;
      for (uint32_t b = 0; b < 11; ++b) {
        imgs[b] = {sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[b + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b + 1].dstSet = sky_sets_[idx];
        writes[b + 1].dstBinding = b + 1;
        writes[b + 1].descriptorCount = 1;
        writes[b + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[b + 1].pImageInfo = &imgs[b];
      }
      vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
    }
  }

  // Create sky pipelines (base opaque + layers premultiplied alpha) & starfield point-list pipeline.
  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(r_.device_, &ci, nullptr, &m);
  };
  VkShaderModule sv = VK_NULL_HANDLE, sf = VK_NULL_HANDLE, stv = VK_NULL_HANDLE, stf = VK_NULL_HANDLE;
  VK_TRY(make_mod(kSky_vert_spv, sizeof(kSky_vert_spv), sv), "sky.vert");
  VK_TRY(make_mod(kSky_frag_spv, sizeof(kSky_frag_spv), sf), "sky.frag");
  VK_TRY(make_mod(kStars_vert_spv, sizeof(kStars_vert_spv), stv), "stars.vert");
  VK_TRY(make_mod(kStars_frag_spv, sizeof(kStars_frag_spv), stf), "stars.frag");

  const VkBool32 linear_out = r_.post() ? VK_TRUE : VK_FALSE;
  const VkBool32 rev_z = r_.reverse_z() ? VK_TRUE : VK_FALSE;
  const VkSpecializationMapEntry spec_lin{0, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_frag{1, &spec_lin, sizeof(VkBool32), &linear_out};
  const VkSpecializationMapEntry spec_rev{1, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_vert{1, &spec_rev, sizeof(VkBool32), &rev_z};

  const VkFormat fmt = r_.scene_format();
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &fmt;
  rend.depthAttachmentFormat = r_.depth_format_;

  VkPipelineVertexInputStateCreateInfo empty_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo tri_ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  tri_ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = r_.samples_;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  // Sky & stars run AFTER opaque geometry at far depth (0.0 reverse-Z, 1.0 forward-Z)
  // so Hardware Early-Z rejects all occluded pixels before sky.frag executes.
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  const VkPipelineShaderStageCreateInfo sky_stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sv, "main",
       &spec_vert},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sf, "main",
       &spec_frag}};

  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &rend;
  gp.stageCount = 2;
  gp.pStages = sky_stages;
  gp.pVertexInputState = &empty_vi;
  gp.pInputAssemblyState = &tri_ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = sky_pipe_layout_;
  VkResult res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &sky_pipe_base_);

  att.blendEnable = VK_TRUE;
  att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.colorBlendOp = VK_BLEND_OP_ADD;
  att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.alphaBlendOp = VK_BLEND_OP_ADD;
  if (res == VK_SUCCESS)
    res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &sky_pipe_layers_);

  // 1-sample HDR sky pipelines & descriptor sets for dynamic cubemap and rearview mirror passes.
  if (res == VK_SUCCESS && dyn_env_ready_) {
    const VkFormat hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipelineRenderingCreateInfo env_rend = rend;
    env_rend.pColorAttachmentFormats = &hdr_fmt;
    VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkGraphicsPipelineCreateInfo egp = gp;
    egp.pNext = &env_rend;
    egp.pMultisampleState = &ms1;
    VkPipelineColorBlendAttachmentState eatt{};
    eatt.colorWriteMask = att.colorWriteMask;
    VkPipelineColorBlendStateCreateInfo ecb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    ecb.attachmentCount = 1;
    ecb.pAttachments = &eatt;
    egp.pColorBlendState = &ecb;
    res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &egp, nullptr, &env_sky_pipe_base_);
    if (res == VK_SUCCESS) {
      eatt = att;
      res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &egp, nullptr, &env_sky_pipe_layers_);
    }
    if (res == VK_SUCCESS) {
      const VkDescriptorPoolSize eps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * 11}};
      VkDescriptorPoolCreateInfo epci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      epci.maxSets = 12;
      epci.poolSizeCount = 2;
      epci.pPoolSizes = eps;
      if (vkCreateDescriptorPool(r_.device_, &epci, nullptr, &env_pool_) == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo ealloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ealloc.descriptorPool = env_pool_;
        ealloc.descriptorSetCount = 12;
        ealloc.pSetLayouts = set_layouts;
        if (vkAllocateDescriptorSets(r_.device_, &ealloc, env_sky_sets_) == VK_SUCCESS) {
          for (int mode = 0; mode < 2; ++mode) {
            for (int disc = 0; disc < 6; ++disc) {
              const int idx = mode * 6 + disc;
              VkDescriptorBufferInfo buf{env_sky_ubo_[mode].buffer, 0, sizeof(SkyUniforms)};
              const VkImageView views[11] = {
                  disc == 0 ? sky_sun_.view : sky_moons_[disc - 1].view,
                  sky_dither_.view,
                  sky_far_f_.view,
                  sky_far_b_.view,
                  sky_far_mask_.view,
                  sky_close_ad_f_.view,
                  sky_close_ad_b_.view,
                  sky_close_am_f_.view,
                  sky_close_am_b_.view,
                  sky_close_tb_f_.view,
                  sky_close_tb_b_.view,
              };
              VkDescriptorImageInfo imgs[11];
              VkWriteDescriptorSet writes[12]{};
              writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
              writes[0].dstSet = env_sky_sets_[idx];
              writes[0].dstBinding = 0;
              writes[0].descriptorCount = 1;
              writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
              writes[0].pBufferInfo = &buf;
              for (uint32_t b = 0; b < 11; ++b) {
                imgs[b] = {sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[b + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[b + 1].dstSet = env_sky_sets_[idx];
                writes[b + 1].dstBinding = b + 1;
                writes[b + 1].descriptorCount = 1;
                writes[b + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b + 1].pImageInfo = &imgs[b];
              }
              vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
            }
          }
        }
      }
    }
  }

  // Starfield pipeline (point list, premultiplied alpha over sky atmosphere).
  VkVertexInputBindingDescription star_bind{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription star_attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo star_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  star_vi.vertexBindingDescriptionCount = 1;
  star_vi.pVertexBindingDescriptions = &star_bind;
  star_vi.vertexAttributeDescriptionCount = 1;
  star_vi.pVertexAttributeDescriptions = &star_attr;
  VkPipelineInputAssemblyStateCreateInfo pt_ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  pt_ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  const VkPipelineShaderStageCreateInfo star_stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, stv, "main",
       &spec_vert},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, stf, "main",
       &spec_frag}};
  gp.pStages = star_stages;
  gp.pVertexInputState = &star_vi;
  gp.pInputAssemblyState = &pt_ia;
  gp.layout = stars_pipe_layout_;
  if (res == VK_SUCCESS)
    res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &stars_pipe_);

  vkDestroyShaderModule(r_.device_, sv, nullptr);
  vkDestroyShaderModule(r_.device_, sf, nullptr);
  vkDestroyShaderModule(r_.device_, stv, nullptr);
  vkDestroyShaderModule(r_.device_, stf, nullptr);
  VK_TRY(res, "sky pipelines");

  sky_ready_ = true;
  return true;
}

void Showroom::fill_sky_uniforms(const ShowroomFrame& view_frame, SkyUniforms& u) const {
  // Compute clip_from_game = view_frame.view_projection * world_transform_ (column-major).
  float clip[16];
  multiply(view_frame.view_projection, world_transform_, clip);
  // Invert 3x3 matrix A = [row0(x); row1(y); row3(w)] (columns 0..2) so
  // V_game = inv(A) * [ndc.x, ndc.y, 1]^T maps screen NDC to exact game-frame ray direction.
  const double a00 = clip[0], a01 = clip[4], a02 = clip[8];
  const double a10 = clip[1], a11 = clip[5], a12 = clip[9];
  const double a20 = clip[3], a21 = clip[7], a22 = clip[11];
  const double c00 = a11 * a22 - a12 * a21;
  const double c01 = -(a10 * a22 - a12 * a20);
  const double c02 = a10 * a21 - a11 * a20;
  const double det = a00 * c00 + a01 * c01 + a02 * c02;
  double inv[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  if (std::fabs(det) > 1e-12) {
    const double id = 1.0 / det;
    inv[0][0] = c00 * id;
    inv[1][0] = c01 * id;
    inv[2][0] = c02 * id;
    inv[0][1] = -(a01 * a22 - a02 * a21) * id;
    inv[1][1] = (a00 * a22 - a02 * a20) * id;
    inv[2][1] = -(a00 * a21 - a01 * a20) * id;
    inv[0][2] = (a01 * a12 - a02 * a11) * id;
    inv[1][2] = -(a00 * a12 - a02 * a10) * id;
    inv[2][2] = (a00 * a11 - a01 * a10) * id;
  }

  const auto& t = tod_state_;
  u = {};
  // V = cam_fwd + ndc.x * cam_right - ndc.y * cam_up = C2 + ndc.x * C0 + ndc.y * C1.
  u.cam_fwd[0] = float(inv[0][2]);
  u.cam_fwd[1] = float(inv[1][2]);
  u.cam_fwd[2] = float(inv[2][2]);
  u.cam_fwd[3] = 0.0f;
  u.cam_right[0] = float(inv[0][0]);
  u.cam_right[1] = float(inv[1][0]);
  u.cam_right[2] = float(inv[2][0]);
  u.cam_right[3] = t.tod_sec / 86400.0f;
  u.cam_up[0] = float(-inv[0][1]);
  u.cam_up[1] = float(-inv[1][1]);
  u.cam_up[2] = float(-inv[2][1]);
  u.cam_up[3] = tod_real_time_;

  u.sun_dir[0] = t.sun_dir[0]; u.sun_dir[1] = t.sun_dir[1]; u.sun_dir[2] = t.sun_dir[2]; u.sun_dir[3] = t.sky_gain;
  const float disc_rad = std::clamp(t.sun_object_size * 0.0032f, 0.025f, 0.11f);
  u.sun_obj_dir[0] = t.sun_object_dir[0]; u.sun_obj_dir[1] = t.sun_object_dir[1];
  u.sun_obj_dir[2] = t.sun_object_dir[2]; u.sun_obj_dir[3] = disc_rad;
  u.haze_dir[0] = t.haze_dir[0]; u.haze_dir[1] = t.haze_dir[1]; u.haze_dir[2] = t.haze_dir[2];
  u.haze_dir[3] = t.is_moon ? 1.0f : 0.0f;

  u.atmo_top[0] = t.atmo_top[0]; u.atmo_top[1] = t.atmo_top[1]; u.atmo_top[2] = t.atmo_top[2]; u.atmo_top[3] = t.atmo_color_power;
  u.atmo_bottom[0] = t.atmo_bottom[0]; u.atmo_bottom[1] = t.atmo_bottom[1]; u.atmo_bottom[2] = t.atmo_bottom[2]; u.atmo_bottom[3] = t.atmo_clamp_lo;
  u.atmo_haze[0] = t.atmo_haze_color[0]; u.atmo_haze[1] = t.atmo_haze_color[1]; u.atmo_haze[2] = t.atmo_haze_color[2]; u.atmo_haze[3] = t.atmo_clamp_hi;
  u.atmo_haze_p[0] = t.atmo_haze_power; u.atmo_haze_p[1] = t.atmo_haze_intensity;
  u.atmo_haze_p[2] = t.atmo_haze_elliptical; u.atmo_haze_p[3] = t.atmo_haze_alpha;

  u.sun_obj_col[0] = t.sun_object_color[0]; u.sun_obj_col[1] = t.sun_object_color[1];
  u.sun_obj_col[2] = t.sun_object_color[2]; u.sun_obj_col[3] = t.sun_object_alpha;

  u.far_dense[0] = t.far_dense[0]; u.far_dense[1] = t.far_dense[1]; u.far_dense[2] = t.far_dense[2]; u.far_dense[3] = t.far_thin_intensity;
  u.far_thin[0] = t.far_thin[0]; u.far_thin[1] = t.far_thin[1]; u.far_thin[2] = t.far_thin[2]; u.far_thin[3] = t.far_alpha_power;
  u.far_params[0] = t.far_centre_alpha; u.far_params[1] = t.far_edge_alpha;
  u.far_params[2] = t.far_coverage; u.far_params[3] = t.far_back_coverage;
  u.far_back_dense[0] = t.far_back_dense[0]; u.far_back_dense[1] = t.far_back_dense[1];
  u.far_back_dense[2] = t.far_back_dense[2]; u.far_back_dense[3] = t.far_back_centre_alpha;
  u.far_back_thin[0] = t.far_back_thin[0]; u.far_back_thin[1] = t.far_back_thin[1];
  u.far_back_thin[2] = t.far_back_thin[2]; u.far_back_thin[3] = t.far_back_edge_alpha;
  const float day_frac = t.tod_sec / 86400.0f;
  u.far_scroll[0] = std::fmod(tod_real_time_ * 0.0012f + day_frac * 0.4f, 1.0f);
  u.far_scroll[1] = std::fmod(tod_real_time_ * 0.0008f, 1.0f);
  u.far_scroll[2] = std::fmod(t.far_back_speed_x * 0.000015f * tod_real_time_, 1.0f);
  u.far_scroll[3] = std::fmod(t.far_back_speed_z * 0.000015f * tod_real_time_, 1.0f);

  u.close_top_col[0] = t.close_top_color[0]; u.close_top_col[1] = t.close_top_color[1];
  u.close_top_col[2] = t.close_top_color[2]; u.close_top_col[3] = t.close_top_power;
  u.close_bot_col[0] = t.close_bottom_color[0]; u.close_bot_col[1] = t.close_bottom_color[1];
  u.close_bot_col[2] = t.close_bottom_color[2]; u.close_bot_col[3] = t.close_bottom_power;
  u.close_back_col[0] = t.close_back_color[0]; u.close_back_col[1] = t.close_back_color[1];
  u.close_back_col[2] = t.close_back_color[2]; u.close_back_col[3] = t.close_back_power;
  u.close_dense_col[0] = t.close_dense_color[0]; u.close_dense_col[1] = t.close_dense_color[1];
  u.close_dense_col[2] = t.close_dense_color[2]; u.close_dense_col[3] = t.close_dense_intensity;
  u.close_thin_col[0] = t.close_thin_color[0]; u.close_thin_col[1] = t.close_thin_color[1];
  u.close_thin_col[2] = t.close_thin_color[2]; u.close_thin_col[3] = t.close_thin_intensity;
  u.close_intens[0] = t.close_back_intensity; u.close_intens[1] = t.close_top_intensity;
  u.close_intens[2] = t.close_bottom_intensity; u.close_intens[3] = t.close_alpha_curve;
  u.close_masks[0] = t.close_top_mask_power; u.close_masks[1] = t.close_bottom_mask_power;
  u.close_masks[2] = t.close_top_alpha; u.close_masks[3] = t.close_bottom_alpha;
  u.close_layers[0] = t.close_front_alpha; u.close_layers[1] = t.close_back_alpha;
  u.close_layers[2] = std::fmod(t.close_front_revs * day_frac + tod_real_time_ * 0.0005f, 1.0f);
  u.close_layers[3] = std::fmod(t.close_back_revs * day_frac - tod_real_time_ * 0.0003f, 1.0f);

  u.fog_color[0] = t.fog_color[0]; u.fog_color[1] = t.fog_color[1]; u.fog_color[2] = t.fog_color[2];
  u.fog_color[3] = std::clamp(t.fog_sky_height_fade_start / 15000.0f, 0.0f, 0.25f);
  u.fog_scatter[0] = t.fog_sun_scatter_color[0]; u.fog_scatter[1] = t.fog_sun_scatter_color[1];
  u.fog_scatter[2] = t.fog_sun_scatter_color[2]; u.fog_scatter[3] = t.fog_sun_scatter_power;
  u.fog_sky[0] = std::clamp(t.fog_sky_height_fade_end / 15000.0f, 0.05f, 0.35f);
  u.fog_sky[1] = std::clamp(t.fog_density * 14.0f, 0.15f, 0.85f);
  u.fog_sky[2] = sky_has_textures_ ? 1.0f : 0.0f;
  u.fog_sky[3] = t.star_fade;
}

void Showroom::record_sky_before(VkCommandBuffer cmd) {
  SkyUniforms u{};
  fill_sky_uniforms(frame_, u);

  // Update mode 0 (atmosphere) and mode 1 (disc + clouds + horizon fog) UBOs.
  for (int m = 0; m < 2; ++m) {
    u.cam_fwd[3] = float(m);
    vkCmdUpdateBuffer(cmd, sky_ubo_[m].buffer, 0, sizeof(SkyUniforms), &u);
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = sky_ubo_[m].buffer;
    b.size = sizeof(SkyUniforms);
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
}

void Showroom::record_sky_draw(VkCommandBuffer cmd) {
  if (!sky_ready_) return;
  const int disc = tod_state_.is_moon ? (1 + tod_moon_phase_) : 0;

  // 1. Atmosphere gradient dome + elliptical sun haze.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_base_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &sky_sets_[disc], 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // 2. Stars.bin starfield point sprites (drawn only when StarFade > 0.005).
  if (star_count_ > 0 && tod_state_.star_fade > 0.005f) {
    struct {
      float view_proj_rot[16];
      float params[4];
    } spc{};
    multiply(frame_.view_projection, world_transform_, spc.view_proj_rot);
    spc.params[0] = tod_state_.star_fade;
    // Slow sidereal rotation: 15 deg/hour = 2*pi / 24h.
    spc.params[1] = tod_state_.tod_sec * (6.2831853f / 86400.0f);
    spc.params[2] = 2.6f;
    spc.params[3] = 1.0f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, stars_pipe_);
    vkCmdPushConstants(cmd, stars_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(spc), &spc);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &star_vertices_.buffer, &zero);
    vkCmdDraw(cmd, star_count_, 1, 0, 0);
  }

  // 3. Sun/Moon disc + Far clouds (2 layers) + Close lit clouds (2 layers) + Horizon fog skirt.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layers_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &sky_sets_[6 + disc], 0,
                          nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Showroom::load_car_lamps(const std::filesystem::path& game_root, const std::string& car) {
  lamp_points_.clear();
  lamp_colors_ = drive::LampColorTable{};
  if (game_root.empty() || car.empty()) return;
  const std::string car_l = lower(car);
  std::filesystem::path zip_path = game_root / "media" / "cars" / (car_l + ".zip");
  if (!std::filesystem::exists(zip_path)) {
    zip_path = game_root / "media" / "cars" / (car + ".zip");
  }
  if (!std::filesystem::exists(zip_path)) return;
  std::string zerr;
  auto ar = fh1::Archive::open(zip_path, &zerr);
  if (!ar) return;
  for (const auto& m : ar->members()) {
    const std::string ml = lower(m.name);
    if (ends_with(ml, "shadersettings.xml")) {
      auto r = ar->read(m);
      if (r.ok()) {
        const std::string xml(reinterpret_cast<const char*>(r.data.data()), r.data.size());
        lamp_colors_.load_shader_settings_xml(xml);
      }
    } else if (ends_with(ml, ".carbin") && ml.find("_cockpit") == std::string::npos &&
               ml.find("_lod0") == std::string::npos && ml.find("_damage") == std::string::npos &&
               lamp_points_.empty()) {
      auto r = ar->read(m);
      if (r.ok()) {
        auto pts = fh1::parse_carbin_lamp_points(r.data);
        if (!pts.empty()) lamp_points_ = std::move(pts);
      }
    }
  }
  if (lamp_points_.empty()) {
    for (const auto& m : ar->members()) {
      const std::string ml = lower(m.name);
      if (ends_with(ml, ".carbin")) {
        auto r = ar->read(m);
        if (r.ok()) {
          auto pts = fh1::parse_carbin_lamp_points(r.data);
          if (!pts.empty()) {
            lamp_points_ = std::move(pts);
            break;
          }
        }
      }
    }
  }
}

void Showroom::update_frame_cookie_descriptor() {
  if (!r_.device_ || !headlight_cookie_.view) return;
  for (VkDescriptorSet target : {frame_set_, shadow_set_, shadow_set_mid_, shadow_set_far_, env_frame_set_}) {
    if (!target) continue;
    VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = target;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &cookie;
    vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
  }
}

bool Showroom::upload_carlights(const std::filesystem::path& media_dir, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (carlights_ready_) return true;

  auto load_xds_rgba = [&](const std::filesystem::path& path, GpuTexture& out, bool fallback_radial) -> bool {
    bake::Texture tex;
    tex.format = bake::TextureFormat::kRgba8;
    std::vector<uint8_t> raw;
    fh1::RgbaImage img;
    bool decoded = false;
    if (std::filesystem::exists(path)) {
      if (FILE* f = std::fopen(path.string().c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        if (sz > 0) {
          raw.resize(size_t(sz));
          std::fseek(f, 0, SEEK_SET);
          if (std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
            auto xr = fh1::decode_xds(raw);
            if (!xr.levels.empty() && xr.levels[0].width > 0 && xr.levels[0].height > 0 &&
                xr.levels[0].rgba.size() == size_t(xr.levels[0].width) * xr.levels[0].height * 4) {
              img = std::move(xr.levels[0]);
              decoded = true;
            }
          }
        }
        std::fclose(f);
      }
    }
    if (decoded) {
      tex.levels.push_back({img.width, img.height, std::move(img.rgba)});
    } else if (fallback_radial) {
      constexpr uint32_t kS = 32;
      std::vector<uint8_t> px(kS * kS * 4);
      for (uint32_t y = 0; y < kS; ++y) {
        for (uint32_t x = 0; x < kS; ++x) {
          const float u = (float(x) + 0.5f) / float(kS) * 2.0f - 1.0f;
          const float v = (float(y) + 0.5f) / float(kS) * 2.0f - 1.0f;
          const float r = std::sqrt(u * u + v * v);
          const float a = std::pow(std::clamp(1.0f - r, 0.0f, 1.0f), 2.2f);
          const uint8_t b = uint8_t(std::lround(a * 255.0f));
          const size_t idx = (size_t(y) * kS + x) * 4;
          px[idx + 0] = b;
          px[idx + 1] = b;
          px[idx + 2] = b;
          px[idx + 3] = b;
        }
      }
      tex.levels.push_back({kS, kS, std::move(px)});
    } else {
      tex.levels.push_back({1, 1, {255, 255, 255, 255}});
    }
    return upload_texture(&tex, false, out, error);
  };

  const auto carlights = media_dir / "carlights";
  // Replace the 1x1 default headlight cookie with media/carlights/headlight_beam_dip.xds.
  GpuTexture cookie;
  if (load_xds_rgba(carlights / "headlight_beam_dip.xds", cookie, true)) {
    r_.wait_idle();
    r_.destroy_image(headlight_cookie_);
    headlight_cookie_ = cookie;
    update_frame_cookie_descriptor();
  }

  for (int i = 0; i < 7; ++i) {
    const std::string name = "headlight_glow_" + std::to_string(i + 1) + ".xds";
    if (!load_xds_rgba(carlights / name, glow_tex_[i], true)) return false;
  }

  if (!r_.create_buffer(kMaxGlowSprites * sizeof(GlowSpriteGpu),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, glow_buffer_)) {
    error = "glow instance buffer";
    return false;
  }

  // Descriptor set layout for glow_tex[7] (set 1, binding 0).
  const VkDescriptorSetLayoutBinding glow_binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_ci.bindingCount = 1;
  set_ci.pBindings = &glow_binding;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set_ci, nullptr, &glow_set_layout_), "glow set layout");

  const VkDescriptorSetLayout pl_sets[2] = {frame_layout_, glow_set_layout_};
  VkPipelineLayoutCreateInfo pl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_ci.setLayoutCount = 2;
  pl_ci.pSetLayouts = pl_sets;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &pl_ci, nullptr, &glow_pipe_layout_), "glow pipeline layout");

  const VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7};
  VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_ci.maxSets = 1;
  pool_ci.poolSizeCount = 1;
  pool_ci.pPoolSizes = &psize;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &pool_ci, nullptr, &glow_pool_), "glow descriptor pool");

  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = glow_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &glow_set_layout_;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &glow_set_), "glow descriptor set");

  VkDescriptorImageInfo imgs[7];
  for (int i = 0; i < 7; ++i)
    imgs[i] = {sampler_, glow_tex_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = glow_set_;
  write.dstBinding = 0;
  write.descriptorCount = 7;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = imgs;
  vkUpdateDescriptorSets(r_.device_, 1, &write, 0, nullptr);

  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(r_.device_, &ci, nullptr, &m);
  };
  VkShaderModule gv = VK_NULL_HANDLE, gf = VK_NULL_HANDLE;
  VK_TRY(make_mod(kGlow_vert_spv, sizeof(kGlow_vert_spv), gv), "glow.vert");
  VK_TRY(make_mod(kGlow_frag_spv, sizeof(kGlow_frag_spv), gf), "glow.frag");

  const VkBool32 linear_out = r_.post() ? VK_TRUE : VK_FALSE;
  const VkSpecializationMapEntry spec_lin{0, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_frag{1, &spec_lin, sizeof(VkBool32), &linear_out};

  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, gv, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, gf, "main",
       &spec_frag}};

  VkVertexInputBindingDescription vbind{0, sizeof(GlowSpriteGpu), VK_VERTEX_INPUT_RATE_INSTANCE};
  const VkVertexInputAttributeDescription vattrs[4] = {
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, pos_radius)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, dir_inner)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, color_outer)},
      {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, extra)}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &vbind;
  vi.vertexAttributeDescriptionCount = 4;
  vi.pVertexAttributeDescriptions = vattrs;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = r_.samples_;
  // Depth-tested against scene depth, depth write OFF (CAR_LAMPS.md §4.3).
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  // Additive blend: SRC_ALPHA / ONE.
  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  att.blendEnable = VK_TRUE;
  att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  att.colorBlendOp = VK_BLEND_OP_ADD;
  att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  att.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  const VkFormat fmt = r_.scene_format();
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &fmt;
  rend.depthAttachmentFormat = r_.depth_format_;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &rend;
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = glow_pipe_layout_;
  const VkResult res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &glow_pipe_);
  vkDestroyShaderModule(r_.device_, gv, nullptr);
  vkDestroyShaderModule(r_.device_, gf, nullptr);
  VK_TRY(res, "glow pipeline");

  carlights_ready_ = true;
  return true;
}

void Showroom::record_lights_before(VkCommandBuffer cmd) {
  auto transform_dir = [](const float m[16], const std::array<float, 3>& v) -> std::array<float, 3> {
    std::array<float, 3> r = {m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
                              m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
                              m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (len > 1e-6f) r[0] /= len, r[1] /= len, r[2] /= len;
    return r;
  };
  auto build_beam = [&](const float render_from_body[16], const std::vector<fh1::CarbinLampPoint>& pts,
                        const drive::LampShaderVectors& vecs, HeadlightBeamGpu& out) -> bool {
    const float head_amt = vecs.dynamic_lights_2[0];
    if (head_amt <= 0.01f) return false;
    const bool fullbeam = vecs.dynamic_lights_2[1] > 0.5f;
    const auto spec = drive::resolve_headlight_beam(pts, fullbeam);
    const auto origin = transform_point(render_from_body, spec.origin_body);
    const float pitch_rad = (fullbeam ? -2.2f : -6.2f) * (3.14159265f / 180.0f);
    const std::array<float, 3> fwd_b = {0.0f, std::sin(pitch_rad), std::cos(pitch_rad)};
    const std::array<float, 3> up_b = {0.0f, std::cos(pitch_rad), -std::sin(pitch_rad)};
    const std::array<float, 3> right_b = {1.0f, 0.0f, 0.0f};
    const auto fwd = transform_dir(render_from_body, fwd_b);
    const auto up = transform_dir(render_from_body, up_b);
    const auto right = transform_dir(render_from_body, right_b);
    const float tan_x = std::tan((fullbeam ? 36.0f : 42.0f) * (3.14159265f / 180.0f));
    const float tan_y = std::tan((fullbeam ? 18.0f : 19.0f) * (3.14159265f / 180.0f));
    const float dx = -(right[0] * origin[0] + right[1] * origin[1] + right[2] * origin[2]);
    const float dy = -(up[0] * origin[0] + up[1] * origin[1] + up[2] * origin[2]);
    const float dz = -(fwd[0] * origin[0] + fwd[1] * origin[1] + fwd[2] * origin[2]);
    // Column-major projection: c.x = x_cam / tan_x, c.y = -y_cam / tan_y, c.z = z_cam, c.w = z_cam.
    float* p = out.proj;
    p[0] = right[0] / tan_x;  p[4] = right[1] / tan_x;  p[8] = right[2] / tan_x;   p[12] = dx / tan_x;
    p[1] = -up[0] / tan_y;    p[5] = -up[1] / tan_y;    p[9] = -up[2] / tan_y;     p[13] = -dy / tan_y;
    p[2] = fwd[0];            p[6] = fwd[1];            p[10] = fwd[2];            p[14] = dz;
    p[3] = fwd[0];            p[7] = fwd[1];            p[11] = fwd[2];            p[15] = dz;
    out.origin_range[0] = origin[0];
    out.origin_range[1] = origin[1];
    out.origin_range[2] = origin[2];
    out.origin_range[3] = spec.range_m;
    out.dir_power[0] = fwd[0];
    out.dir_power[1] = fwd[1];
    out.dir_power[2] = fwd[2];
    out.dir_power[3] = (fullbeam ? 7.5f : 5.8f) * head_amt;
    out.color[0] = spec.color[0];
    out.color[1] = spec.color[1];
    out.color[2] = spec.color[2];
    out.color[3] = 1.0f;
    return true;
  };

  // .carbin lamp points live in car model space (same vertical origin as body mesh,
  // only Z-mirror/pitch rotation from body_to_model_ applies; no lift offset!).
  auto lamp_to_model = [](const float btm[16], float out[16]) {
    std::memcpy(out, btm, 16 * sizeof(float));
    out[12] = out[13] = out[14] = 0.0f;
  };
  HeadlightUbo hl{};
  const float night_factor = tod_active_ ? std::clamp(tod_state_.switch_on_lights, 0.0f, 1.0f) : 0.0f;
  int beam_count = 0;
  // Player headlight beam first (when car drawn and headlamps active).
  if (draw_car_ && world_lit_) {
    float ltm[16];
    lamp_to_model(body_to_model_, ltm);
    if (build_beam(ltm, lamp_points_, lamp_vectors_, hl.beams[beam_count])) ++beam_count;
  }
  // Nearest traffic / AI vehicles with headlamps on (up to 16 total beams).
  struct Cand {
    float dist2;
    const Instance* inst;
  };
  std::vector<Cand> cands;
  cands.reserve(instances_.size());
  for (const auto& in : instances_) {
    if (!in.car || !in.has_lamps || in.lamps.dynamic_lights_2[0] <= 0.01f) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < 180.0f * 180.0f) cands.push_back({d2, &in});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
  for (const auto& c : cands) {
    if (beam_count >= 16) break;
    float ltm[16], rfb[16];
    lamp_to_model(c.inst->car->body_to_model_, ltm);
    multiply(c.inst->model, ltm, rfb);
    if (build_beam(rfb, c.inst->car->lamp_points_, c.inst->lamps, hl.beams[beam_count])) ++beam_count;
  }
  // Enable road beam projection at dusk/night, or softly in daylight if user forces fullbeam/lowbeam override.
  const float beam_gate = std::max(night_factor, lamp_vectors_.dynamic_lights_2[1] > 0.5f ? 0.35f : 0.0f);
  hl.count_night[0] = float(beam_count);
  hl.count_night[1] = beam_gate;
  hl.count_night[2] = tod_real_time_;
  hl.count_night[3] = 0.0f;
  // Mirror nearest up to 8 active headlight cones for Phase B 3D volumetric headlight scattering.
  vol_beam_count_ = uint32_t(std::min(beam_count, 8));
  vol_night_factor_ = beam_gate;
  for (uint32_t bi = 0; bi < vol_beam_count_; ++bi) {
    const auto& src_b = hl.beams[bi];
    auto& dst_b = vol_beams_[bi];
    for (int k = 0; k < 4; ++k) {
      dst_b.origin_range[k] = src_b.origin_range[k];
      dst_b.dir_power[k] = src_b.dir_power[k];
    }
    dst_b.color_cos[0] = src_b.color[0];
    dst_b.color_cos[1] = src_b.color[1];
    dst_b.color_cos[2] = src_b.color[2];
    dst_b.color_cos[3] = 0.72f;  // ~44 deg volumetric cone half-angle matching automotive low/high beam
  }

  {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = headlights_ubo_.buffer;
    b.size = sizeof(HeadlightUbo);
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdUpdateBuffer(cmd, headlights_ubo_.buffer, 0, sizeof(HeadlightUbo), &hl);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // Collect additive glow sprites (car .carbin lamp points + world streetlamp/outpost glows).
  glow_count_ = 0;
  if (!carlights_ready_ || !glow_buffer_.buffer) return;
  std::vector<GlowSpriteGpu> sprites;
  sprites.reserve(256);

  auto point_amt = [](const fh1::CarbinLampPoint& pt, const drive::LampShaderVectors& vecs) -> float {
    float a = 0.0f;
    if (pt.side_flags & 0x80u) a = 1.0f;
    if (pt.channel_mask & 0x01u) a = std::max(a, vecs.dynamic_lights_2[0]);
    if (pt.channel_mask & 0x02u) a = std::max(a, vecs.dynamic_lights_2[0] * 0.65f);
    if (pt.channel_mask & 0x04u) a = std::max(a, vecs.dynamic_lights_amount[0]);
    if (pt.channel_mask & 0x08u) {
      if (pt.side_flags & 0x01u) a = std::max(a, vecs.dynamic_lights_amount[2]);
      if (pt.side_flags & 0x02u) a = std::max(a, vecs.dynamic_lights_amount[3]);
    }
    if (pt.channel_mask & 0x10u) a = std::max(a, vecs.dynamic_lights_amount[1]);
    return a;
  };

  auto push_car_glows = [&](const float render_from_body[16], const std::vector<fh1::CarbinLampPoint>& pts,
                            const drive::LampShaderVectors& vecs, float max_dist) {
    for (const auto& pt : pts) {
      if (sprites.size() >= kMaxGlowSprites) break;
      const float amt = point_amt(pt, vecs);
      if (amt <= 0.01f) continue;
      const auto g = drive::resolve_lamp_glow(pt);
      const auto p = transform_point(render_from_body, pt.position);
      const auto d = transform_dir(render_from_body, pt.direction);
      const float inner_cos = std::cos(g.inner_cone_deg * (3.14159265f / 180.0f));
      const float outer_cos = std::cos(g.outer_cone_deg * (3.14159265f / 180.0f));
      // Unclamped HDR glow luminance (> 4.0) drives authentic FH1 crimson taillight & headlamp bloom.
      const float gain = (pt.channel_mask & 0x01u) ? 5.8f
                       : (pt.channel_mask & 0x04u) ? 6.2f
                       : (pt.channel_mask & 0x02u) ? 5.8f
                                                   : 2.5f;
      const float r_scale = (pt.channel_mask & 0x01u) ? 0.26f : 0.45f;
      GlowSpriteGpu s{};
      s.pos_radius[0] = p[0];
      s.pos_radius[1] = p[1];
      s.pos_radius[2] = p[2];
      s.pos_radius[3] = std::clamp(g.radius_m * r_scale, 0.08f, 0.45f);
      s.dir_inner[0] = d[0];
      s.dir_inner[1] = d[1];
      s.dir_inner[2] = d[2];
      s.dir_inner[3] = inner_cos;
      s.color_outer[0] = g.color[0] * gain * amt;
      s.color_outer[1] = g.color[1] * gain * amt;
      s.color_outer[2] = g.color[2] * gain * amt;
      s.color_outer[3] = outer_cos;
      s.extra[0] = float(std::clamp(g.texture_index - 1, 0, 6));
      s.extra[1] = 0.16f;  // bias toward camera (m) to clear lamp lens & bumper lip
      s.extra[2] = max_dist;
      s.extra[3] = 1.0f;
      sprites.push_back(s);
    }
  };

  if (draw_car_ && world_lit_) {
    float ltm[16];
    lamp_to_model(body_to_model_, ltm);
    push_car_glows(ltm, lamp_points_, lamp_vectors_, 250.0f);
  }
  for (const auto& in : instances_) {
    if (!in.car || !in.has_lamps || in.car->lamp_points_.empty()) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    if (dx * dx + dy * dy + dz * dz > 350.0f * 350.0f) continue;
    float ltm[16], rfb[16];
    lamp_to_model(in.car->body_to_model_, ltm);
    multiply(in.model, ltm, rfb);
    push_car_glows(rfb, in.car->lamp_points_, in.lamps, 350.0f);
  }

  // World streetlamp / outpost / festival glow sprites (gated by SwitchOnLights).
  if (world_textured_ && night_factor > 0.01f && !world_glows_.empty()) {
    struct WorldCand {
      float dist2;
      size_t idx;
      float fade;
      std::array<float, 3> pos;
    };
    std::vector<WorldCand> wc;
    wc.reserve(world_glows_.size());
    for (size_t i = 0; i < world_glows_.size(); ++i) {
      const auto& g = world_glows_[i];
      const float fade = std::clamp((night_factor - g.threshold) / 0.14f, 0.0f, 1.0f);
      if (fade <= 0.005f) continue;
      const auto p = transform_point(world_transform_, g.position);
      const float dx = p[0] - frame_.eye[0];
      const float dy = p[1] - frame_.eye[1];
      const float dz = p[2] - frame_.eye[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > 420.0f * 420.0f) continue;
      wc.push_back({d2, i, fade, p});
    }
    std::sort(wc.begin(), wc.end(), [](const WorldCand& a, const WorldCand& b) { return a.dist2 < b.dist2; });
    for (const auto& c : wc) {
      if (sprites.size() >= kMaxGlowSprites) break;
      const auto& g = world_glows_[c.idx];
      GlowSpriteGpu s{};
      s.pos_radius[0] = c.pos[0];
      s.pos_radius[1] = c.pos[1];
      s.pos_radius[2] = c.pos[2];
      s.pos_radius[3] = g.radius;
      s.dir_inner[0] = 0.0f;
      s.dir_inner[1] = 0.0f;
      s.dir_inner[2] = 0.0f;
      s.dir_inner[3] = -1.0f;  // omnidirectional
      s.color_outer[0] = g.color[0] * 2.8f;
      s.color_outer[1] = g.color[1] * 2.8f;
      s.color_outer[2] = g.color[2] * 2.8f;
      s.color_outer[3] = -1.0f;
      s.extra[0] = 0.0f;  // headlight_glow_1.xds soft radial glow
      s.extra[1] = 0.25f;
      s.extra[2] = 420.0f;
      s.extra[3] = c.fade;
      sprites.push_back(s);
    }
  }

  glow_count_ = uint32_t(sprites.size());
  if (glow_count_ > 0) {
    const VkDeviceSize bytes = VkDeviceSize(glow_count_) * sizeof(GlowSpriteGpu);
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = glow_buffer_.buffer;
    b.size = bytes;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    // vkCmdUpdateBuffer limit is 65536 bytes (= 1024 * 64 = kMaxGlowSprites * sizeof(GlowSpriteGpu)).
    vkCmdUpdateBuffer(cmd, glow_buffer_.buffer, 0, bytes, sprites.data());
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
}

void Showroom::record_glows_draw(VkCommandBuffer cmd) {
  if (!carlights_ready_ || glow_count_ == 0 || !glow_pipe_) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_);
  const VkDescriptorSet sets[2] = {frame_set_, glow_set_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_layout_, 0, 2, sets, 0, nullptr);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &glow_buffer_.buffer, &zero);
  vkCmdDraw(cmd, 6, glow_count_, 0, 0);
}

bool Showroom::init_dynamic_reflections(std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (dyn_env_ready_) return true;

  // RGBA16F cubemap (256^2x9 high, 128^2x8 low-power) for live drive reflections (car + LAKE water).
  const VkFormat hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
  const uint32_t env_sz = dyn_env_size_;
  const uint32_t env_mips = std::min(dyn_env_mips_, kDynEnvMips);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = hdr_fmt;
  ci.extent = {env_sz, env_sz, 1};
  ci.mipLevels = env_mips;
  ci.arrayLayers = 6;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo ainfo{};
  VK_TRY(vmaCreateImage(r_.allocator_, &ci, &alloc, &dyn_env_.image, &dyn_env_.allocation, &ainfo),
         "dynamic env cubemap");
  dyn_env_.bytes = ainfo.size;

  VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  cv.image = dyn_env_.image;
  cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  cv.format = hdr_fmt;
  cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, env_mips, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &cv, nullptr, &dyn_env_.view), "dynamic env cube view");

  VkImageViewCreateInfo cv0{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  cv0.image = dyn_env_.image;
  cv0.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  cv0.format = hdr_fmt;
  cv0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &cv0, nullptr, &dyn_env_mip0_cube_), "dynamic env mip0 cube view");

  for (uint32_t f = 0; f < 6; ++f) {
    VkImageViewCreateInfo fv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    fv.image = dyn_env_.image;
    fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    fv.format = hdr_fmt;
    fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
    VK_TRY(vkCreateImageView(r_.device_, &fv, nullptr, &dyn_env_faces_[f]), "dynamic env face view");
    for (uint32_t m = 1; m < env_mips; ++m) {
      fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, f, 1};
      VK_TRY(vkCreateImageView(r_.device_, &fv, nullptr, &dyn_env_mip_faces_[m][f]), "dynamic env mip face view");
    }
  }

  // Cross-face GGX cubemap prefilter pipeline (post.vert + env_prefilter.frag).
  {
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dsl, nullptr, &env_prefilter_set_layout_), "prefilter dsl");
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &env_prefilter_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pl, nullptr, &env_prefilter_pipe_layout_), "prefilter pl");
    VkShaderModule pv = VK_NULL_HANDLE, pf = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(kPost_vert_spv);
    smi.pCode = kPost_vert_spv;
    VK_TRY(vkCreateShaderModule(r_.device_, &smi, nullptr, &pv), "prefilter vert");
    smi.codeSize = sizeof(kEnv_prefilter_frag_spv);
    smi.pCode = kEnv_prefilter_frag_spv;
    if (vkCreateShaderModule(r_.device_, &smi, nullptr, &pf) != VK_SUCCESS) {
      vkDestroyShaderModule(r_.device_, pv, nullptr);
      error = "prefilter frag";
      return false;
    }
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pf, "main",
         nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl.attachmentCount = 1;
    bl.pAttachments = &att;
    const VkDynamicState dyn_s[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_s;
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &hdr_fmt;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rend;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &bl;
    pci.pDynamicState = &dyn;
    pci.layout = env_prefilter_pipe_layout_;
    VkResult pr_res =
        vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &pci, nullptr, &env_prefilter_pipe_);
    vkDestroyShaderModule(r_.device_, pv, nullptr);
    vkDestroyShaderModule(r_.device_, pf, nullptr);
    VK_TRY(pr_res, "prefilter pipeline");
  }

  if (!r_.create_image(env_sz, env_sz, 1, r_.depth_format_, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, dyn_env_depth_)) {
    error = "dynamic env depth";
    return false;
  }

  if (!r_.create_buffer(sizeof(ShowroomFrame),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, env_frame_buffer_) ||
      !r_.create_buffer(sizeof(SkyUniforms),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, env_sky_ubo_[0]) ||
      !r_.create_buffer(sizeof(SkyUniforms),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, env_sky_ubo_[1])) {
    error = "dynamic env buffers";
    return false;
  }

  // Clear cubemap to SHADER_READ_ONLY_OPTIMAL so descriptors are valid before first draw.
  // Cockpit mirror targets are allocated lazily on first cockpit camera activation.
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dyn_env_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, env_mips, 0, 6};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue clr{{0.08f, 0.10f, 0.14f, 1.0f}};
    vkCmdClearColorImage(cmd, dyn_env_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    db.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = dyn_env_depth_.image;
    db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo ddep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    ddep.imageMemoryBarrierCount = 1;
    ddep.pImageMemoryBarriers = &db;
    vkCmdPipelineBarrier2(cmd, &ddep);
  });
  if (!ok) {
    error = "dynamic env clear";
    return false;
  }
  dyn_env_ready_ = true;
  if (mirror_enabled_) ensure_mirror_targets();
  return true;
}

bool Showroom::ensure_mirror_targets() {
  if (guest_ || !r_.device_) return false;
  if (mirror_tex_.view && mirror_depth_.view) return true;
  const VkFormat hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
  if (!r_.create_image(kMirrorWidth, kMirrorHeight, 1, hdr_fmt, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, mirror_tex_) ||
      !r_.create_image(kMirrorWidth, kMirrorHeight, 1, r_.depth_format_, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, mirror_depth_)) {
    r_.destroy_image(mirror_tex_);
    r_.destroy_image(mirror_depth_);
    return false;
  }
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mirror_tex_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue clr{{0.08f, 0.10f, 0.14f, 1.0f}};
    vkCmdClearColorImage(cmd, mirror_tex_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    db.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = mirror_depth_.image;
    db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo ddep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    ddep.imageMemoryBarrierCount = 1;
    ddep.pImageMemoryBarriers = &db;
    vkCmdPipelineBarrier2(cmd, &ddep);
  });
  if (!ok) {
    r_.destroy_image(mirror_tex_);
    r_.destroy_image(mirror_depth_);
    return false;
  }
  if (frame_set_ && sampler_) {
    r_.wait_idle();
    VkDescriptorImageInfo info{sampler_, mirror_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = frame_set_;
    w.dstBinding = 5;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
  }
  mirror_valid_ = false;
  return true;
}

void Showroom::record_offscreen_view(VkCommandBuffer cmd, VkImageView color_view, VkImageView depth_view,
                                     uint32_t w, uint32_t h, const ShowroomFrame& view_frame, bool include_cars,
                                     uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh, bool clear_target,
                                     uint32_t* draw_counter) {
  if (vw == 0) vw = w;
  if (vh == 0) vh = h;
  // Upload view UBO and sky UBOs outside render pass.
  update_buffer(cmd, env_frame_buffer_.buffer, view_frame);
  if (sky_ready_ && env_sky_pipe_base_ && env_sky_sets_[0]) {
    SkyUniforms u{};
    fill_sky_uniforms(view_frame, u);
    for (int m = 0; m < 2; ++m) {
      u.cam_fwd[3] = float(m);
      vkCmdUpdateBuffer(cmd, env_sky_ubo_[m].buffer, 0, sizeof(SkyUniforms), &u);
      VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = env_sky_ubo_[m].buffer;
      b.size = sizeof(SkyUniforms);
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
  }

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = color_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = clear_target ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{0.05f, 0.08f, 0.12f, 1.0f}};
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {r_.reverse_z() ? 0.0f : 1.0f, 0};
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{int32_t(vx), int32_t(vy)}, {vw, vh}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &color;
  rend.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rend);
  const VkViewport vp{float(vx), float(vy), float(vw), float(vh), 0.0f, 1.0f};
  const VkRect2D sc{{int32_t(vx), int32_t(vy)}, {vw, vh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);

  // 1. Opaque AND alpha-tested (blend 0 + blend 1) world terrain, roads, mountains, trees,
  //    buildings, signs, banners, and barriers (drawn BEFORE sky for Early-Z rejection).
  if (world_textured_ && !world_draws_.empty() && env_world_opaque_ && env_frame_set_) {
    float clip[16];
    multiply(view_frame.view_projection, world_transform_, clip);
    float planes[4][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], wv = clip[c * 4 + 3];
      planes[0][c] = wv + x, planes[1][c] = wv - x, planes[2][c] = wv + y, planes[3][c] = wv - y;
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
    const auto eye_sc = [&]() -> std::array<float, 3> {
      const float e[3] = {view_frame.eye[0] - world_transform_[12], view_frame.eye[1] - world_transform_[13],
                          view_frame.eye[2] - world_transform_[14]};
      std::array<float, 3> out{};
      for (int c = 0; c < 3; ++c)
        out[size_t(c)] = world_transform_[c * 4 + 0] * e[0] + world_transform_[c * 4 + 1] * e[1] +
                         world_transform_[c * 4 + 2] * e[2];
      return out;
    }();
    const float max_dist = include_cars ? (low_memory_mode_ ? 200.0f : 320.0f) : 160.0f;
    const float max_dist2 = max_dist * max_dist;
    const float cutout_dist2 = include_cars ? (low_memory_mode_ ? 110.0f * 110.0f : 165.0f * 165.0f) : (75.0f * 75.0f);
    const float layer_dist2 = include_cars ? 120.0f * 120.0f : 80.0f * 80.0f;
    world_bind_buffers(cmd, false);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &env_frame_set_, 0,
                            nullptr);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    VkPipeline bound_pipe = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    for (const auto& d : world_draws_) {
      // Include opaque (blend 0) AND H_DIFF*/TREE_* alpha-tested props/trees/banners (blend 1).
      if (!d.cull || d.blend > 1) continue;
      // Fast squared AABB distance cull before frustum planes (cuts ~3,400 draws down to ~150-250).
      float d2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float q = std::max({d.box_min[k] - eye_sc[size_t(k)], 0.0f, eye_sc[size_t(k)] - d.box_max[k]});
        d2 += q * q;
      }
      if (d2 > max_dist2) continue;
      if (d.blend == 1 && d2 > cutout_dist2) continue;
      if (outside(d)) continue;
      const uint32_t fl = uint32_t(d.push.params[1] + 0.5f);
      if ((fl & (2u | 32768u)) != 0u) continue;
      VkPipeline want_pipe = (d.blend == 1 && env_world_coverage_) ? env_world_coverage_ : env_world_opaque_;
      if (want_pipe != bound_pipe)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipe = want_pipe);

      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      uint32_t out_fl = fl;
      const bool has_extra = (d2 <= layer_dist2) &&
                             (d.texture_b != kNoWorldTexture || d.splat != kNoWorldTexture ||
                              d.ao_map != kNoWorldTexture || d.layer_c != kNoWorldTexture ||
                              d.light_map != kNoWorldTexture || d.spec_map != kNoWorldTexture);
      if ((set || d.texture == kNoWorldTexture) && has_extra) {
        VkDescriptorSet layered = world_layer_set(
            {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map});
        if (layered) {
          set = layered;
        } else {
          out_fl &= ~(4u | 8u | 4096u | 8192u | 16384u | 524288u);
        }
      } else if (!has_extra) {
        out_fl &= ~(4u | 8u | 2048u | 4096u | 8192u | 16384u | 524288u);
      }
      if (!set) {
        set = world_white_set_;
        out_fl &= ~1u;
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &(bound_set = set),
                                0, nullptr);
      ShowroomPush push = d.push;
      // 1x offscreen target has no MSAA alpha-to-coverage: enable alpha test (flag 64) for blend==1.
      if (d.blend == 1) out_fl |= 64u;
      push.params[1] = float(out_fl);
      world_draw_model(d, world_transform_, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = 2.0f;
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      if (draw_counter) ++(*draw_counter);
    }
  }
  // 2. Trailing AI / traffic cars in cockpit mirrors (when include_cars is set).
  if (include_cars && env_car_opaque_ && env_frame_set_) {
    for (const auto& in : instances_) {
      if (!in.car) continue;
      const float dx = in.model[12] - view_frame.eye[0];
      const float dy = in.model[13] - view_frame.eye[1];
      const float dz = in.model[14] - view_frame.eye[2];
      if (dx * dx + dy * dy + dz * dz > 220.0f * 220.0f) continue;
      in.car->record_instance_offscreen(cmd, env_car_opaque_, env_frame_set_, in.model,
                                        in.has_lamps ? &in.lamps : nullptr);
      if (draw_counter) *draw_counter += uint32_t(in.car->draw_count());
    }
  }
  // 3. Real-time sky AFTER opaque world & cars (Early-Z rejects occluded pixels).
  if (sky_ready_ && env_sky_pipe_base_ && env_sky_pipe_layers_ && env_sky_sets_[0]) {
    const int disc = tod_state_.is_moon ? (1 + tod_moon_phase_) : 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_sky_pipe_base_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &env_sky_sets_[disc], 0,
                            nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_sky_pipe_layers_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &env_sky_sets_[6 + disc], 0,
                            nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  vkCmdEndRendering(cmd);
}

void Showroom::record_env_and_mirror(VkCommandBuffer cmd) {
  env_draws_ = 0;
  mirror_draws_ = 0;
  if (!dyn_env_ready_ || !env_frame_set_) return;
  auto make_view_proj = [&](const std::array<float, 3>& eye, const std::array<float, 3>& right,
                            const std::array<float, 3>& up, const std::array<float, 3>& fwd,
                            float tan_x, float tan_y, float out[16]) {
    const float dx = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    const float dy = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    const float dz = -(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    const float near_p = 0.5f, far_p = 8000.0f;
    out[0] = right[0] / tan_x; out[4] = right[1] / tan_x; out[8]  = right[2] / tan_x; out[12] = dx / tan_x;
    out[1] = -up[0] / tan_y;   out[5] = -up[1] / tan_y;   out[9]  = -up[2] / tan_y;   out[13] = -dy / tan_y;
    if (r_.reverse_z()) {
      out[2] = 0.0f; out[6] = 0.0f; out[10] = 0.0f; out[14] = near_p;
    } else {
      const float k = far_p / (far_p - near_p);
      out[2] = fwd[0] * k; out[6] = fwd[1] * k; out[10] = fwd[2] * k; out[14] = dz * k - near_p * k;
    }
    out[3] = fwd[0]; out[7] = fwd[1]; out[11] = fwd[2]; out[15] = dz;
  };
  auto game_to_render = [&](const std::array<float, 3>& g) -> std::array<float, 3> {
    return {world_transform_[0] * g[0] + world_transform_[4] * g[1] + world_transform_[8] * g[2],
            world_transform_[1] * g[0] + world_transform_[5] * g[1] + world_transform_[9] * g[2],
            world_transform_[2] * g[0] + world_transform_[6] * g[1] + world_transform_[10] * g[2]};
  };

  // Vulkan cubemap face basis in game space (+X, -X, +Y, -Y, +Z, -Z).
  static constexpr std::array<float, 3> kFwd[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static constexpr std::array<float, 3> kRight[6] = {
      {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
  static constexpr std::array<float, 3> kUp[6] = {
      {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};

  // Schedule dynamic cubemap face updates per preset.reflection_rate and stagger 3-band cockpit mirrors.
  const modern::ReflectionStep rstep = modern::schedule_reflections(
      world_frame_, reflection_rate_, dyn_env_initialized_, mirror_enabled_, mirror_valid_);
  const uint32_t env_sz = dyn_env_size_;
  const uint32_t env_mips = std::min(dyn_env_mips_, kDynEnvMips);
  // Probe center slightly above car roof in render space.
  const std::array<float, 3> cube_eye = {0.0f, 1.35f, 0.0f};
  // Pass A: Render updated face(s) into mip 0 and transition mip 0 to SHADER_READ_ONLY_OPTIMAL.
  if (rstep.render_cubemap) {
    for (uint32_t i = 0; i < rstep.face_count; ++i) {
      const uint32_t f = (rstep.first_face + i) % 6;
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = dyn_env_.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);

      ShowroomFrame vf = frame_;
      vf.shadow[0] = 0.0f;
      vf.shadow_far_bias[1] = vf.shadow_far_bias[2] = vf.shadow_far_bias[3] = 0.0f;
      vf.eye[0] = cube_eye[0];
      vf.eye[1] = cube_eye[1];
      vf.eye[2] = cube_eye[2];
      vf.eye[3] = 1.0f;
      make_view_proj(cube_eye, game_to_render(kRight[f]), game_to_render(kUp[f]), game_to_render(kFwd[f]),
                     1.0f, 1.0f, vf.view_projection);
      std::memcpy(vf.transparent_view_projection, vf.view_projection, sizeof vf.view_projection);

      record_offscreen_view(cmd, dyn_env_faces_[f], dyn_env_depth_.view, env_sz, env_sz, vf, false,
                            0u, 0u, 0u, 0u, true, &env_draws_);

      b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
    r_.mark(cmd, "env_cubemap");

    // Pass B: Cross-face GGX prefilter mips 1..env_mips-1 sampling mip 0 across all 6 faces
    // seamlessly (eliminating cube seams on rough/metallic car paint).
    if (env_prefilter_pipe_ && env_prefilter_set_) {
      for (uint32_t i = 0; i < rstep.face_count; ++i) {
        const uint32_t f = (rstep.first_face + i) % 6;
        uint32_t mip_w = env_sz;
        for (uint32_t mip = 1; mip < env_mips; ++mip) {
          mip_w = std::max(1u, mip_w / 2u);
          VkImageMemoryBarrier2 mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
          mb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
          mb.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
          mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
          mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
          mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          mb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          mb.image = dyn_env_.image;
          mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, f, 1};
          VkDependencyInfo mdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          mdep.imageMemoryBarrierCount = 1;
          mdep.pImageMemoryBarriers = &mb;
          vkCmdPipelineBarrier2(cmd, &mdep);

          VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          ca.imageView = dyn_env_mip_faces_[mip][f];
          ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
          ri.renderArea = {{0, 0}, {mip_w, mip_w}};
          ri.layerCount = 1;
          ri.colorAttachmentCount = 1;
          ri.pColorAttachments = &ca;
          vkCmdBeginRendering(cmd, &ri);
          const VkViewport vp{0, 0, float(mip_w), float(mip_w), 0.0f, 1.0f};
          const VkRect2D sc{{0, 0}, {mip_w, mip_w}};
          vkCmdSetViewport(cmd, 0, 1, &vp);
          vkCmdSetScissor(cmd, 0, 1, &sc);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_prefilter_pipe_);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_prefilter_pipe_layout_, 0, 1,
                                  &env_prefilter_set_, 0, nullptr);
          const float rough = float(mip) / float(env_mips - 1u);
          const float pp[4] = {float(f), rough, float(mip), 1.0f / float(mip_w)};
          vkCmdPushConstants(cmd, env_prefilter_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
          vkCmdDraw(cmd, 3, 1, 0, 0);
          vkCmdEndRendering(cmd);

          mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
          mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
          mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
          mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
          mb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          vkCmdPipelineBarrier2(cmd, &mdep);
        }
      }
    }
    r_.mark(cmd, "env_prefilter");
    dyn_env_initialized_ = true;
  }

  // Dedicated 3-band cockpit mirror pass (1024x256) ONLY when cockpit camera is active:
  // Band 0 [0..341): mirrorLeft, Band 1 [341..683): mirrorMiddle, Band 2 [683..1024): mirrorRight.
  // Staggers 1 band/frame after initial full 3-band populate (preserving untouched bands).
  if (rstep.render_mirror && mirror_tex_.view && mirror_depth_.view) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = mirror_valid_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mirror_tex_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    struct MirrorBand {
      uint32_t x, w;
      std::array<float, 3> eye, fwd;
      float tx, ty;
    };
    const float sl = 1.0f / std::sqrt(0.20f * 0.20f + 0.04f * 0.04f + 0.98f * 0.98f);
    const MirrorBand bands[3] = {
        {0u, 341u, {-0.95f, 0.95f, 0.25f}, {-0.20f * sl, -0.04f * sl, 0.98f * sl}, 0.50f, 0.28f},
        {341u, 342u, {0.0f, 1.18f, 0.25f}, {0.0f, -0.02f, 0.9998f}, 0.58f, 0.22f},
        {683u, 341u, {0.95f, 0.95f, 0.25f}, {0.20f * sl, -0.04f * sl, 0.98f * sl}, 0.50f, 0.28f},
    };
    for (uint32_t bi = 0; bi < rstep.mirror_band_count; ++bi) {
      const auto& mb = bands[(rstep.mirror_band_begin + bi) % 3u];
      // right = cross(fwd, up=(0,1,0)) = (-fwd.z, 0, fwd.x)
      const std::array<float, 3> mright = {-mb.fwd[2], 0.0f, mb.fwd[0]};
      ShowroomFrame mf = frame_;
      mf.shadow[0] = 0.0f;
      mf.shadow_far_bias[1] = mf.shadow_far_bias[2] = mf.shadow_far_bias[3] = 0.0f;
      mf.eye[0] = mb.eye[0];
      mf.eye[1] = mb.eye[1];
      mf.eye[2] = mb.eye[2];
      mf.eye[3] = 1.0f;
      make_view_proj(mb.eye, mright, {0.0f, 1.0f, 0.0f}, mb.fwd, mb.tx, mb.ty, mf.view_projection);
      std::memcpy(mf.transparent_view_projection, mf.view_projection, sizeof mf.view_projection);
      record_offscreen_view(cmd, mirror_tex_.view, mirror_depth_.view, kMirrorWidth, kMirrorHeight, mf, true,
                            mb.x, 0u, mb.w, kMirrorHeight, true, &mirror_draws_);
    }

    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    mirror_valid_ = true;
    r_.mark(cmd, "cockpit_mirrors");
  } else if (!mirror_enabled_) {
    mirror_valid_ = false;
  }
}

namespace {

void blit_cell_bilinear(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t atlas_w,
                        uint32_t cell_x, uint32_t cell_y, uint32_t cell_sz) {
  if (!src || sw == 0 || sh == 0) return;
  for (uint32_t y = 0; y < cell_sz; ++y) {
    const float fy = (float(y) + 0.5f) * float(sh) / float(cell_sz) - 0.5f;
    const int y0 = std::clamp(int(std::floor(fy)), 0, int(sh) - 1);
    const int y1 = std::clamp(y0 + 1, 0, int(sh) - 1);
    const float wy = std::clamp(fy - float(y0), 0.0f, 1.0f);
    for (uint32_t x = 0; x < cell_sz; ++x) {
      const float fx = (float(x) + 0.5f) * float(sw) / float(cell_sz) - 0.5f;
      const int x0 = std::clamp(int(std::floor(fx)), 0, int(sw) - 1);
      const int x1 = std::clamp(x0 + 1, 0, int(sw) - 1);
      const float wx = std::clamp(fx - float(x0), 0.0f, 1.0f);
      const uint8_t* p00 = src + (size_t(y0) * sw + size_t(x0)) * 4;
      const uint8_t* p10 = src + (size_t(y0) * sw + size_t(x1)) * 4;
      const uint8_t* p01 = src + (size_t(y1) * sw + size_t(x0)) * 4;
      const uint8_t* p11 = src + (size_t(y1) * sw + size_t(x1)) * 4;
      uint8_t* o = dst + (size_t(cell_y * cell_sz + y) * atlas_w + size_t(cell_x * cell_sz + x)) * 4;
      for (int c = 0; c < 4; ++c) {
        const float top = float(p00[c]) * (1.0f - wx) + float(p10[c]) * wx;
        const float bot = float(p01[c]) * (1.0f - wx) + float(p11[c]) * wx;
        o[c] = uint8_t(std::clamp(top * (1.0f - wy) + bot * wy + 0.5f, 0.0f, 255.0f));
      }
    }
  }
}

void fill_soft_puff_cell(uint8_t* dst, uint32_t atlas_w, uint32_t cell_sz) {
  for (uint32_t y = 0; y < cell_sz; ++y) {
    const float ny = (float(y) + 0.5f) / float(cell_sz) * 2.0f - 1.0f;
    for (uint32_t x = 0; x < cell_sz; ++x) {
      const float nx = (float(x) + 0.5f) / float(cell_sz) * 2.0f - 1.0f;
      const float r = std::sqrt(nx * nx + ny * ny);
      const float ang = std::atan2(ny, nx);
      const float billow = 0.88f + 0.12f * std::sin(ang * 5.0f + r * 3.0f) * std::cos(ang * 3.0f - r * 2.0f);
      const float a = std::clamp(1.0f - r / billow, 0.0f, 1.0f);
      const float soft = a * a * (3.0f - 2.0f * a);
      const uint8_t shade = uint8_t(std::clamp(210.0f + 45.0f * soft, 0.0f, 255.0f));
      uint8_t* o = dst + (size_t(y) * atlas_w + size_t(x)) * 4;
      o[0] = shade;
      o[1] = shade;
      o[2] = shade;
      o[3] = uint8_t(std::clamp(soft * 245.0f, 0.0f, 255.0f));
    }
  }
}

std::vector<uint8_t> make_skid_texture(uint32_t sz, bool is_rut) {
  std::vector<uint8_t> rgba(size_t(sz) * sz * 4, 0);
  auto hash2 = [](int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
  };
  for (uint32_t y = 0; y < sz; ++y) {
    const float v = float(y) / float(sz);
    for (uint32_t x = 0; x < sz; ++x) {
      const float u = float(x) / float(sz);
      const float edge = std::sin(std::clamp(u, 0.0f, 1.0f) * 3.14159265f);
      const float n = hash2(int(x / 2), int(y / 2)) * 0.35f + hash2(int(x), int(y)) * 0.15f;
      uint8_t* o = rgba.data() + (size_t(y) * sz + size_t(x)) * 4;
      if (!is_rut) {
        // Asphalt rubber: 4 longitudinal tyre-tread ribs with feathered edges.
        const float ribs = 0.65f + 0.35f * std::pow(std::fabs(std::sin(u * 4.0f * 3.14159265f)), 0.45f);
        const float streak = 0.85f + 0.15f * std::sin(v * 12.566f + u * 25.0f);
        const float a = std::clamp(std::pow(edge, 0.45f) * ribs * streak * (0.85f + 0.2f * n), 0.0f, 1.0f);
        o[0] = 28;
        o[1] = 28;
        o[2] = 30;
        o[3] = uint8_t(a * 240.0f);
      } else {
        // Dirt/gravel rut: darker depressed centre + pale dusty shoulder ridges + tread chevrons.
        const float chev = 0.80f + 0.20f * std::sin((v * 14.0f + std::fabs(u - 0.5f) * 8.0f) * 6.28318f);
        const float ridge = std::exp(-std::pow((std::fabs(u - 0.5f) - 0.38f) / 0.12f, 2.0f));
        const float shade = std::clamp(0.62f + 0.32f * ridge + 0.12f * n, 0.0f, 1.0f) * chev;
        const float a = std::clamp(std::pow(edge, 0.55f) * (0.78f + 0.22f * ridge) * (0.82f + 0.25f * n), 0.0f, 1.0f);
        o[0] = uint8_t(std::clamp(shade * 235.0f, 0.0f, 255.0f));
        o[1] = uint8_t(std::clamp(shade * 212.0f, 0.0f, 255.0f));
        o[2] = uint8_t(std::clamp(shade * 180.0f, 0.0f, 255.0f));
        o[3] = uint8_t(a * 225.0f);
      }
    }
  }
  return rgba;
}

}  // namespace

bool Showroom::init_vfx(const world::ParticleLibrary* lib, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (vfx_ready_) return true;

  // 1. Build 2048x2048 RGBA8 particle texture atlas (8x8 grid of 256x256 cells).
  std::vector<uint8_t> atlas_rgba(size_t(kParticleAtlasSize) * kParticleAtlasSize * 4, 0);
  fill_soft_puff_cell(atlas_rgba.data(), kParticleAtlasSize, kParticleAtlasCell);
  atlas_tex_slot_.clear();

  if (lib) {
    // Prioritize core vehicle/world textures first, then fill remaining slots up to 63.
    std::vector<std::string> tex_order = {
        "gr_puffy_smoke_diff.xds",   "gr_backfire_diff.xds",     "vehicle_sparks_diff.xds",
        "gr_dirt_8x1_diff.xds",      "gr_gravel_8x1_diff.xds",   "gr_woodsplinters_diff.xds",
        "gr_aspen_leaves_diff.xds",  "gr_grass_8x1_diff.xds",    "gr_glass_diff.xds",
    };
    for (const auto& [ename, def] : lib->effects()) {
      if (!def.texture_name.empty() &&
          std::find(tex_order.begin(), tex_order.end(), def.texture_name) == tex_order.end()) {
        tex_order.push_back(def.texture_name);
      }
    }
    uint16_t slot = 1;
    const uint16_t max_slots = uint16_t(kParticleAtlasGrid * kParticleAtlasGrid);
    for (const auto& tname : tex_order) {
      if (slot >= max_slots) break;
      fh1::RgbaImage img;
      if (lib->decode_texture(tname, img)) {
        const uint32_t cx = slot % kParticleAtlasGrid;
        const uint32_t cy = slot / kParticleAtlasGrid;
        blit_cell_bilinear(img.rgba.data(), img.width, img.height, atlas_rgba.data(), kParticleAtlasSize, cx, cy,
                           kParticleAtlasCell);
        atlas_tex_slot_[tname] = slot;
        ++slot;
      }
    }
  }

  auto upload_rgba_image = [&](const uint8_t* pixels, uint32_t w, uint32_t h, bool srgb, GpuTexture& dst,
                               const char* label) -> bool {
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    Renderer::Buffer staging;
    if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
      error = std::string(label) + " staging";
      return false;
    }
    void* mapped = nullptr;
    if (vmaMapMemory(r_.allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
      r_.destroy_buffer(staging);
      error = std::string(label) + " map";
      return false;
    }
    std::memcpy(mapped, pixels, size_t(bytes));
    vmaUnmapMemory(r_.allocator_, staging.allocation);
    const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!r_.create_image(w, h, 1, fmt, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         dst)) {
      r_.destroy_buffer(staging);
      error = std::string(label) + " image";
      return false;
    }
    const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = dst.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
      VkBufferImageCopy reg{};
      reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      reg.imageExtent = {w, h, 1};
      vkCmdCopyBufferToImage(cmd, staging.buffer, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
      b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier2(cmd, &dep);
    });
    r_.destroy_buffer(staging);
    if (!ok) error = std::string(label) + " upload";
    return ok;
  };

  if (!upload_rgba_image(atlas_rgba.data(), kParticleAtlasSize, kParticleAtlasSize, true, particle_atlas_,
                         "particle atlas"))
    return false;
  const auto skid_asph = make_skid_texture(256, false);
  const auto skid_rut = make_skid_texture(256, true);
  if (!upload_rgba_image(skid_asph.data(), 256, 256, true, skid_asphalt_tex_, "skid asphalt")) return false;
  if (!upload_rgba_image(skid_rut.data(), 256, 256, true, skid_rut_tex_, "skid rut")) return false;

  // 2. Samplers.
  VkSamplerCreateInfo psamp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  psamp.magFilter = psamp.minFilter = VK_FILTER_LINEAR;
  psamp.addressModeU = psamp.addressModeV = psamp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(r_.device_, &psamp, nullptr, &particle_sampler_), "particle sampler");

  VkSamplerCreateInfo ssamp = psamp;
  ssamp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ssamp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  ssamp.anisotropyEnable = r_.sampler_anisotropy() > 1.0f ? VK_TRUE : VK_FALSE;
  ssamp.maxAnisotropy = r_.sampler_anisotropy();
  VK_TRY(vkCreateSampler(r_.device_, &ssamp, nullptr, &skid_sampler_), "skid sampler");

  // 3. Mapped host-visible dynamic buffers.
  if (!create_dyn_buffer(particle_buf_, VkDeviceSize(kMaxParticleGpu) * sizeof(ParticleGpu), error)) {
    error = "particle instance buffer";
    return false;
  }
  if (!create_dyn_buffer(skid_asphalt_buf_, VkDeviceSize(kMaxSkidVerts) * sizeof(SkidGpuVertex), error) ||
      !create_dyn_buffer(skid_rut_buf_, VkDeviceSize(kMaxSkidVerts) * sizeof(SkidGpuVertex), error) ||
      !create_dyn_buffer(debris_buf_, VkDeviceSize(kMaxDebrisVerts) * sizeof(WorldVertex), error) ||
      !create_dyn_buffer(driver_buf_, VkDeviceSize(kMaxDriverVerts) * sizeof(WorldVertex), error)) {
    error = "skidmark/debris/driver vertex buffers";
    return false;
  }
  if (!options_.game_root.empty()) {
    std::string spec_err;
    if (spectator_anim_.init(options_.game_root / "media", world::TrackPostConfig{}, &spec_err) &&
        spectator_anim_.catalog().has_3d_spectators()) {
      const auto& cat = spectator_anim_.catalog();
      if (upload_rgba_image(cat.atlas_rgba.data(), world::SpectatorCatalog::kAtlasSize,
                            world::SpectatorCatalog::kAtlasSize, true, spectator_atlas_, "spectator atlas") &&
          create_world_single_set(spectator_atlas_.view, spectator_set_, spec_err) &&
          create_dyn_buffer(spectator_buf_, VkDeviceSize(kMaxSpectatorVerts) * sizeof(WorldVertex), spec_err)) {
        // Authentic Spectators.zip 3D crowd atlas + dynamic vertex buffer ready.
      }
    }
  }
  update_driver_mesh(driver_wheel_rad_);

  // 4. Descriptor sets & pipeline layouts.
  {
    const VkDescriptorSetLayoutBinding pb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = pb;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dli, nullptr, &particle_set_layout_), "particle set layout");

    const VkDescriptorSetLayout sets[2] = {frame_layout_, particle_set_layout_};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pli, nullptr, &particle_pipe_layout_), "particle pipe layout");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_TRY(vkCreateDescriptorPool(r_.device_, &dpi, nullptr, &particle_pool_), "particle pool");

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = particle_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &particle_set_layout_;
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &dai, &particle_set_), "particle set");

    VkDescriptorImageInfo atlas_info{particle_sampler_, particle_atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depth_fallback{particle_sampler_, gpu_textures_[0].view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = particle_set_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &atlas_info;
    w[1].dstSet = particle_set_;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &depth_fallback;
    vkUpdateDescriptorSets(r_.device_, 2, w, 0, nullptr);
    particle_bound_depth_ = VK_NULL_HANDLE;
  }

  {
    const VkDescriptorSetLayoutBinding sb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                          nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &sb;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dli, nullptr, &skid_set_layout_), "skid set layout");

    const VkDescriptorSetLayout sets[2] = {frame_layout_, skid_set_layout_};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pli, nullptr, &skid_pipe_layout_), "skid pipe layout");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_TRY(vkCreateDescriptorPool(r_.device_, &dpi, nullptr, &skid_pool_), "skid pool");

    const VkDescriptorSetLayout layouts[2] = {skid_set_layout_, skid_set_layout_};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = skid_pool_;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &dai, skid_sets_), "skid sets");

    VkDescriptorImageInfo imgs[2] = {
        {skid_sampler_, skid_asphalt_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {skid_sampler_, skid_rut_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int i = 0; i < 2; ++i) {
      w[i].dstSet = skid_sets_[i];
      w[i].dstBinding = 0;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(r_.device_, 2, w, 0, nullptr);
  }

  // 5. Graphics pipelines (particle alpha + additive, and skidmark).
  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(r_.device_, &ci, nullptr, &m);
  };
  const VkBool32 spec_vals[2] = {r_.post() ? VK_TRUE : VK_FALSE, r_.reverse_z() ? VK_TRUE : VK_FALSE};
  const VkSpecializationMapEntry spec_map[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
  VkSpecializationInfo spec{2, spec_map, sizeof(spec_vals), spec_vals};
  VkSpecializationInfo spec1{1, spec_map, sizeof(VkBool32), spec_vals};

  const VkFormat color_fmt = r_.scene_format();
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &color_fmt;
  rend.depthAttachmentFormat = r_.depth_format_;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = r_.samples_;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  {
    VkShaderModule pv = VK_NULL_HANDLE, pf = VK_NULL_HANDLE;
    VK_TRY(make_mod(kParticle_vert_spv, sizeof(kParticle_vert_spv), pv), "particle vert");
    VK_TRY(make_mod(kParticle_frag_spv, sizeof(kParticle_frag_spv), pf), "particle frag");
    const VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pf, "main",
         &spec},
    };
    const VkVertexInputBindingDescription bind{0, sizeof(ParticleGpu), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[6] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},  {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32}, {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 64}, {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 80},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rend;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = particle_pipe_layout_;
    VkResult r1 =
        vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &particle_alpha_pipe_);
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkResult r2 = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &particle_add_pipe_);
    vkDestroyShaderModule(r_.device_, pv, nullptr);
    vkDestroyShaderModule(r_.device_, pf, nullptr);
    VK_TRY(r1, "particle alpha pipeline");
    VK_TRY(r2, "particle additive pipeline");
  }

  {
    VkShaderModule sv = VK_NULL_HANDLE, sf = VK_NULL_HANDLE;
    VK_TRY(make_mod(kSkid_vert_spv, sizeof(kSkid_vert_spv), sv), "skid vert");
    VK_TRY(make_mod(kSkid_frag_spv, sizeof(kSkid_frag_spv), sf), "skid frag");
    const VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sf, "main",
         &spec1},
    };
    const VkVertexInputBindingDescription bind{0, sizeof(SkidGpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 32},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineRasterizationStateCreateInfo srs = rs;
    srs.depthBiasEnable = VK_TRUE;
    srs.depthBiasConstantFactor = r_.reverse_z() ? kDecalBiasConstant : -kDecalBiasConstant;
    srs.depthBiasSlopeFactor = r_.reverse_z() ? kDecalBiasSlope : -kDecalBiasSlope;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rend;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &srs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = skid_pipe_layout_;
    VkResult sr = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &skid_pipe_);
    vkDestroyShaderModule(r_.device_, sv, nullptr);
    vkDestroyShaderModule(r_.device_, sf, nullptr);
    VK_TRY(sr, "skid pipeline");
  }

  vfx_ready_ = true;
  return true;
}

void Showroom::set_particles(const std::vector<world::ParticleSprite>& alpha,
                             const std::vector<world::ParticleSprite>& additive,
                             const std::vector<std::string>& used_textures, float near_plane, bool cockpit_camera) {
  particle_alpha_count_ = 0;
  particle_add_count_ = 0;
  particle_near_plane_ = std::max(near_plane, 0.05f);
  particle_cockpit_ = cockpit_camera;
  if (!vfx_ready_ || !particle_buf_.ready()) return;

  const float* m = world_transform_;
  auto xform_pt = [&](const std::array<float, 3>& g) -> std::array<float, 3> {
    const float rx = float(double(g[0]) - world_origin_[0]);
    const float ry = g[1];
    const float rz = float(double(g[2]) - world_origin_[1]);
    return {
        m[0] * rx + m[4] * ry + m[8] * rz + m[12],
        m[1] * rx + m[5] * ry + m[9] * rz + m[13],
        m[2] * rx + m[6] * ry + m[10] * rz + m[14],
    };
  };
  auto xform_dir = [&](const std::array<float, 3>& v) -> std::array<float, 3> {
    return {
        m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
        m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
        m[2] * v[0] + m[6] * v[1] + m[10] * v[2],
    };
  };

  auto* dst = static_cast<ParticleGpu*>(particle_buf_.data());
  uint32_t out_n = 0;
  const float cell_uv = 1.0f / float(kParticleAtlasGrid);
  const float inset = 1.5f / float(kParticleAtlasSize);

  auto emit_batch = [&](const std::vector<world::ParticleSprite>& list) -> uint32_t {
    uint32_t added = 0;
    for (const auto& sp : list) {
      if (out_n >= kMaxParticleGpu) break;
      uint16_t atlas_slot = 0;
      if (sp.texture_slot > 0 && size_t(sp.texture_slot - 1) < used_textures.size()) {
        auto it = atlas_tex_slot_.find(used_textures[size_t(sp.texture_slot - 1)]);
        if (it != atlas_tex_slot_.end()) atlas_slot = it->second;
      }
      const uint32_t cx = atlas_slot % kParticleAtlasGrid;
      const uint32_t cy = atlas_slot / kParticleAtlasGrid;
      const int fx = std::max(1, sp.frames_x);
      const int fy = std::max(1, sp.frames_y);
      const int fi = std::max(0, sp.frame_index) % (fx * fy);
      const int fcol = fi % fx;
      const int frow = fi / fx;
      const float sub_u = (cell_uv - 2.0f * inset) / float(fx);
      const float sub_v = (cell_uv - 2.0f * inset) / float(fy);
      const float u0 = float(cx) * cell_uv + inset + float(fcol) * sub_u;
      const float v0 = float(cy) * cell_uv + inset + float(frow) * sub_v;

      const auto rp = xform_pt(sp.pos);
      const auto rv = xform_dir(sp.vel);
      uint32_t flags = (sp.velocity_aligned ? 1u : 0u) | (sp.dynamic_lighting ? 2u : 0u) | (sp.additive ? 4u : 0u);
      const uint32_t scr_byte = uint32_t(std::clamp(sp.max_screen_size * 100.0f, 8.0f, 95.0f));
      flags |= (scr_byte << 8u);

      ParticleGpu& g = dst[out_n++];
      g.pos_sx[0] = rp[0];
      g.pos_sx[1] = rp[1];
      g.pos_sx[2] = rp[2];
      g.pos_sx[3] = sp.size_x;
      g.vel_sy[0] = rv[0];
      g.vel_sy[1] = rv[1];
      g.vel_sy[2] = rv[2];
      g.vel_sy[3] = sp.size_y;
      g.uv_rect[0] = u0;
      g.uv_rect[1] = v0;
      g.uv_rect[2] = sub_u;
      g.uv_rect[3] = sub_v;
      for (int c = 0; c < 4; ++c) {
        g.color[c] = sp.rgba[c];
        g.light[c] = sp.light_influence[c];
      }
      g.extra[0] = sp.spin_rad;
      g.extra[1] = sp.soft_depth_m;
      g.extra[2] = sp.headlight_power;
      g.extra[3] = float(flags);
      ++added;
    }
    return added;
  };

  particle_alpha_count_ = emit_batch(alpha);
  particle_add_count_ = emit_batch(additive);
  ++particle_buf_.ver;
}

void Showroom::set_skidmarks(const std::vector<world::SkidVertex>& asphalt_tris,
                             const std::vector<world::SkidVertex>& rut_tris) {
  skid_asphalt_count_ = 0;
  skid_rut_count_ = 0;
  if (!vfx_ready_ || !skid_asphalt_buf_.ready() || !skid_rut_buf_.ready()) return;

  auto pack = [&](const std::vector<world::SkidVertex>& src, void* mapped) -> uint32_t {
    const uint32_t n = uint32_t(std::min(src.size(), size_t(kMaxSkidVerts)));
    auto* dst = static_cast<SkidGpuVertex*>(mapped);
    for (uint32_t i = 0; i < n; ++i) {
      const auto& v = src[i];
      dst[i].pos[0] = float(double(v.pos[0]) - world_origin_[0]);
      dst[i].pos[1] = v.pos[1];
      dst[i].pos[2] = float(double(v.pos[2]) - world_origin_[1]);
      dst[i].normal[0] = v.normal[0];
      dst[i].normal[1] = v.normal[1];
      dst[i].normal[2] = v.normal[2];
      dst[i].uv[0] = v.uv[0];
      dst[i].uv[1] = v.uv[1];
      for (int c = 0; c < 4; ++c) dst[i].rgba[c] = v.rgba[c];
    }
    return n;
  };
  skid_asphalt_count_ = pack(asphalt_tris, skid_asphalt_buf_.data());
  skid_rut_count_ = pack(rut_tris, skid_rut_buf_.data());
  ++skid_asphalt_buf_.ver;
  ++skid_rut_buf_.ver;
}

void Showroom::set_debris_mesh(const std::vector<world::DebrisVertex>& tris) {
  debris_count_ = 0;
  if (!vfx_ready_ || !debris_buf_.ready() || tris.empty()) return;
  const uint32_t n = uint32_t(std::min(tris.size(), size_t(kMaxDebrisVerts)));
  auto* dst = static_cast<WorldVertex*>(debris_buf_.data());
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };
  for (uint32_t i = 0; i < n; ++i) {
    const auto& v = tris[i];
    WorldVertex& o = dst[i];
    o = WorldVertex{};
    o.position[0] = float(double(v.pos[0]) - world_origin_[0]);
    o.position[1] = v.pos[1];
    o.position[2] = float(double(v.pos[2]) - world_origin_[1]);
    o.normal[0] = snorm8(v.normal[0]);
    o.normal[1] = snorm8(v.normal[1]);
    o.normal[2] = snorm8(v.normal[2]);
    o.normal[3] = 0;
    for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
  }
  debris_count_ = n;
  ++debris_buf_.ver;
}

void Showroom::update_driver_mesh(float wheel_rad) {
  driver_count_ = 0;
  if (!driver_buf_.ready()) return;

  // Steering wheel centre & axis in car model space (+X right, +Y up, -Z forward, +Z rearward).
  std::array<float, 3> pivot{-0.36f, 0.72f, 0.08f};
  std::array<float, 3> axis{0.0f, -0.25f, -0.968f};
  if (cockpit_rigs_[0].valid) {
    pivot = cockpit_rigs_[0].pivot;
    axis = cockpit_rigs_[0].axis;
  }

  auto vadd = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
  };
  auto vsub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  auto vscale = [](const std::array<float, 3>& a, float s) -> std::array<float, 3> {
    return {a[0] * s, a[1] * s, a[2] * s};
  };
  auto vcross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  };
  auto vnorm = [](const std::array<float, 3>& a) -> std::array<float, 3> {
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 1e-6f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : std::array<float, 3>{0, 1, 0};
  };
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };

  auto* dst = static_cast<WorldVertex*>(driver_buf_.data());
  uint32_t count = 0;
  auto push_tri = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                      const std::array<uint8_t, 4>& col) {
    if (count + 3 > kMaxDriverVerts) return;
    // world_opaque_ uses VK_FRONT_FACE_CLOCKWISE (for det=-1 world_transform_); emit CW order
    // (a, c, b) while keeping the outward CCW geometric normal so car-space det=+1 faces draw.
    const auto n = vnorm(vcross(vsub(b, a), vsub(c, a)));
    const std::array<float, 3>* pts[3] = {&a, &c, &b};
    for (int k = 0; k < 3; ++k) {
      WorldVertex& o = dst[count++];
      o = WorldVertex{};
      o.position[0] = (*pts[k])[0];
      o.position[1] = (*pts[k])[1];
      o.position[2] = (*pts[k])[2];
      o.normal[0] = snorm8(n[0]);
      o.normal[1] = snorm8(n[1]);
      o.normal[2] = snorm8(n[2]);
      for (int j = 0; j < 4; ++j) o.colour[j] = col[j];
    }
  };
  auto push_quad = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                       const std::array<float, 3>& d, const std::array<uint8_t, 4>& col) {
    push_tri(a, b, c, col);
    push_tri(a, c, d, col);
  };
  auto push_box = [&](const std::array<float, 3>& c, const std::array<float, 3>& rx, const std::array<float, 3>& ry,
                      const std::array<float, 3>& rz, const std::array<uint8_t, 4>& col) {
    auto pt = [&](float sx, float sy, float sz) -> std::array<float, 3> {
      return {c[0] + rx[0] * sx + ry[0] * sy + rz[0] * sz, c[1] + rx[1] * sx + ry[1] * sy + rz[1] * sz,
              c[2] + rx[2] * sx + ry[2] * sy + rz[2] * sz};
    };
    const auto p000 = pt(-1, -1, -1), p100 = pt(1, -1, -1), p110 = pt(1, 1, -1), p010 = pt(-1, 1, -1);
    const auto p001 = pt(-1, -1, 1), p101 = pt(1, -1, 1), p111 = pt(1, 1, 1), p011 = pt(-1, 1, 1);
    push_quad(p001, p101, p111, p011, col);
    push_quad(p100, p000, p010, p110, col);
    push_quad(p011, p111, p110, p010, col);
    push_quad(p000, p100, p101, p001, col);
    push_quad(p101, p100, p110, p111, col);
    push_quad(p000, p001, p011, p010, col);
  };
  auto push_limb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float rad,
                       const std::array<uint8_t, 4>& col) {
    const auto dir = vsub(b, a);
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-4f) return;
    const auto f = vscale(dir, 0.5f);
    const auto mid = vadd(a, f);
    const auto u = vnorm(dir);
    const std::array<float, 3> ref = std::fabs(u[1]) < 0.9f ? std::array<float, 3>{0, 1, 0} : std::array<float, 3>{1, 0, 0};
    const auto r1 = vscale(vnorm(vcross(ref, u)), rad);
    const auto r2 = vscale(vnorm(vcross(u, r1)), rad);
    push_box(mid, r1, r2, f, col);
  };

  // Orthonormal wheel plane basis perpendicular to steering column axis.
  const auto ax = vnorm(axis);
  const auto wheel_right = vnorm(vcross({0.0f, 1.0f, 0.0f}, vscale(ax, -1.0f)));
  const auto wheel_up = vnorm(vcross(vscale(ax, -1.0f), wheel_right));

  // Clamp hand animation angle to realistic single-grip range (~ +/- 135 deg) so arms never twist through chest.
  const float grip_ang = std::clamp(wheel_rad, -2.35f, 2.35f);
  const float ca = std::cos(grip_ang), sa = std::sin(grip_ang);
  const float rim_r = 0.165f;
  // 9 o'clock (-wheel_right) and 3 o'clock (+wheel_right) rotated by grip_ang.
  const auto l_dir = vadd(vscale(wheel_right, -ca), vscale(wheel_up, sa));
  const auto r_dir = vadd(vscale(wheel_right, ca), vscale(wheel_up, -sa));
  const auto l_hand = vadd(pivot, vscale(l_dir, rim_r));
  const auto r_hand = vadd(pivot, vscale(r_dir, rim_r));

  // Rounded ellipsoid helper for smooth racing helmet & shoulders (no blocky cube helmet!).
  auto push_ellipsoid = [&](const std::array<float, 3>& c, float rx, float ry, float rz,
                            const std::array<uint8_t, 4>& col, int stacks = 5, int slices = 10) {
    constexpr float kPi = 3.14159265358979f;
    auto pt = [&](int st, int sl) -> std::array<float, 3> {
      const float phi = -0.5f * kPi + kPi * float(st) / float(stacks);
      const float th = 2.0f * kPi * float(sl) / float(slices);
      const float cp = std::cos(phi);
      return {c[0] + rx * cp * std::cos(th), c[1] + ry * std::sin(phi), c[2] + rz * cp * std::sin(th)};
    };
    for (int st = 0; st < stacks; ++st) {
      for (int sl = 0; sl < slices; ++sl) {
        const auto p00 = pt(st, sl);
        const auto p10 = pt(st + 1, sl);
        const auto p11 = pt(st + 1, sl + 1);
        const auto p01 = pt(st, sl + 1);
        if (st > 0) push_tri(p00, p10, p11, col);
        if (st + 1 < stacks) push_tri(p00, p11, p01, col);
      }
    }
  };

  // Seated festival driver behind steering wheel (+Z rearward in model space, visible through
  // rear and side glass in chase/external cameras, hidden in cockpit/hood/bumper views).
  const float lean = std::clamp(wheel_rad * 0.03f, -0.045f, 0.045f);
  const std::array<float, 3> chest{pivot[0] + lean * 0.4f, pivot[1] - 0.06f, pivot[2] + 0.36f};
  const std::array<float, 3> head{pivot[0] + lean, pivot[1] + 0.21f, pivot[2] + 0.38f};

  const std::array<uint8_t, 4> jacket{56, 66, 82, 255};
  const std::array<uint8_t, 4> collar{168, 52, 68, 255};
  const std::array<uint8_t, 4> skin{184, 140, 114, 255};
  const std::array<uint8_t, 4> hair{48, 38, 32, 255};
  const std::array<uint8_t, 4> shades{22, 24, 30, 255};
  const std::array<uint8_t, 4> glove{42, 44, 50, 255};

  // Sculpted festival driver torso, collar, neck, head, hair cap, sunglasses & arms.
  push_box(chest, {0.19f, 0, 0}, {0, 0.21f, 0}, {0, 0, 0.105f}, jacket);
  push_box({chest[0], chest[1] + 0.16f, chest[2] - 0.02f}, {0.205f, 0, 0}, {0, 0.048f, 0}, {0, 0, 0.11f}, collar);
  push_box({head[0], head[1] - 0.11f, head[2]}, {0.052f, 0, 0}, {0, 0.060f, 0}, {0, 0, 0.052f}, skin);
  push_ellipsoid(head, 0.098f, 0.115f, 0.112f, skin, 5, 10);
  push_ellipsoid({head[0], head[1] + 0.042f, head[2] + 0.015f}, 0.104f, 0.088f, 0.114f, hair, 5, 10);
  push_ellipsoid({head[0], head[1] + 0.008f, head[2] - 0.084f}, 0.078f, 0.028f, 0.032f, shades, 3, 8);

  // Left & right shoulders -> elbows -> hands gripping the rotating steering wheel rim.
  const std::array<float, 3> l_sh{chest[0] - 0.21f, chest[1] + 0.14f, chest[2] - 0.02f};
  const std::array<float, 3> r_sh{chest[0] + 0.21f, chest[1] + 0.14f, chest[2] - 0.02f};
  const std::array<float, 3> l_el{(l_sh[0] + l_hand[0]) * 0.5f - 0.06f, (l_sh[1] + l_hand[1]) * 0.5f - 0.08f,
                                  (l_sh[2] + l_hand[2]) * 0.5f + 0.04f};
  const std::array<float, 3> r_el{(r_sh[0] + r_hand[0]) * 0.5f + 0.06f, (r_sh[1] + r_hand[1]) * 0.5f - 0.08f,
                                  (r_sh[2] + r_hand[2]) * 0.5f + 0.04f};
  push_limb(l_sh, l_el, 0.048f, jacket);
  push_limb(r_sh, r_el, 0.048f, jacket);
  push_limb(l_el, l_hand, 0.040f, jacket);
  push_limb(r_el, r_hand, 0.040f, jacket);
  push_box(l_hand, {0.042f, 0, 0}, {0, 0.042f, 0}, {0, 0, 0.046f}, glove);
  push_box(r_hand, {0.042f, 0, 0}, {0, 0.042f, 0}, {0, 0, 0.046f}, glove);

  driver_count_ = count;
  ++driver_buf_.ver;
}

void Showroom::record_driver_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || driver_count_ == 0 || !world_opaque_ || !world_white_set_) return;
  const bool draw_player = draw_car_ && !cockpit_camera_active_;
  if (!draw_player && instances_.empty()) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_);
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  r_.apply_vrs(cmd);
  const VkDescriptorSet sets[2] = {frame_set_, world_white_set_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
  VkDeviceSize at = dyn_sync(driver_buf_, VkDeviceSize(driver_count_) * sizeof(WorldVertex));
  vkCmdBindVertexBuffers(cmd, 0, 1, &driver_buf_.buf.buffer, &at);

  ShowroomPush pc{};
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3] * 0.78f;  // balanced cabin-shaded lighting visible through rear/side glass
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;
  pc.params[0] = 0.0f;      // kOpaque
  pc.params[1] = 65536.0f;  // kFlagVertexLit
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;

  if (draw_player) {
    identity(pc.model);  // player driver vertices are in car model space
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDraw(cmd, driver_count_, 1, 0, 0);
  }
  // Also render seated drivers inside nearby AI race / festival / traffic cars.
  for (const auto& in : instances_) {
    if (!in.car) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    if (dx * dx + dy * dy + dz * dz > 95.0f * 95.0f) continue;
    std::memcpy(pc.model, in.model, sizeof pc.model);
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDraw(cmd, driver_count_, 1, 0, 0);
  }
}

void Showroom::record_debris_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || !world_opaque_) return;
  const bool draw_debris = debris_count_ > 0 && world_white_set_;
  const bool draw_spectators =
      spectator_set_ && spectator_buf_.ready() && spectator_anim_.catalog().has_3d_spectators();
  if (!draw_debris && !draw_spectators) return;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_);
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  r_.apply_vrs(cmd);

  ShowroomPush pc{};
  std::memcpy(pc.model, world_transform_, sizeof pc.model);
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3];
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;
  pc.params[0] = 0.0f;  // kOpaque
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;

  if (draw_debris) {
    const VkDescriptorSet sets[2] = {frame_set_, world_white_set_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
    pc.params[1] = 65536.0f;  // kFlagVertexLit
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    VkDeviceSize at = dyn_sync(debris_buf_, VkDeviceSize(debris_count_) * sizeof(WorldVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &debris_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, debris_count_, 1, 0, 0);
  }

  if (draw_spectators) {
    const auto eye = eye_in_scenery(world_transform_);
    const double eye_gx = world_origin_[0] + double(eye[0]);
    const double eye_gy = double(eye[1]);
    const double eye_gz = world_origin_[1] + double(eye[2]);
    spectator_cpu_.clear();
    spectator_anim_.build_spectator_mesh(eye_gx, eye_gy, eye_gz, tod_real_time_, nullptr, spectator_cpu_);
    const uint32_t n = uint32_t(std::min(spectator_cpu_.size(), size_t(kMaxSpectatorVerts)));
    spectator_count_ = n;
    if (n > 0) {
      auto* dst = static_cast<WorldVertex*>(spectator_buf_.data());
      auto snorm8 = [](float x) -> int8_t {
        return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
      };
      for (uint32_t i = 0; i < n; ++i) {
        const auto& v = spectator_cpu_[i];
        WorldVertex& o = dst[i];
        o = WorldVertex{};
        o.position[0] = float(double(v.pos[0]) - world_origin_[0]);
        o.position[1] = v.pos[1];
        o.position[2] = float(double(v.pos[2]) - world_origin_[1]);
        o.normal[0] = snorm8(v.normal[0]);
        o.normal[1] = snorm8(v.normal[1]);
        o.normal[2] = snorm8(v.normal[2]);
        o.normal[3] = 0;
        o.uv0[0] = v.uv[0];
        o.uv0[1] = v.uv[1];
        for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
      }
      ++spectator_buf_.ver;
      const VkDescriptorSet sets[2] = {frame_set_, spectator_set_};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
      pc.params[1] = 1.0f;  // kFlagTextured
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof pc, &pc);
      VkDeviceSize at = dyn_sync(spectator_buf_, VkDeviceSize(n) * sizeof(WorldVertex));
      vkCmdBindVertexBuffers(cmd, 0, 1, &spectator_buf_.buf.buffer, &at);
      vkCmdDraw(cmd, n, 1, 0, 0);
    }
  }
}

void Showroom::record_skidmarks_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || !skid_pipe_ || (skid_asphalt_count_ == 0 && skid_rut_count_ == 0)) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skid_pipe_);
  struct SkidPush {
    float model[16];
    float sun_dir[4];
    float up_dir[4];
  } pc{};
  std::memcpy(pc.model, world_transform_, sizeof pc.model);
  for (int i = 0; i < 3; ++i) {
    pc.sun_dir[i] = world_sun_[i];
    pc.up_dir[i] = world_up_[i];
  }
  pc.sun_dir[3] = world_up_[3];

  if (skid_asphalt_count_ > 0) {
    pc.up_dir[3] = 0.0f;
    const VkDescriptorSet sets[2] = {frame_set_, skid_sets_[0]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skid_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, skid_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    VkDeviceSize at = dyn_sync(skid_asphalt_buf_, VkDeviceSize(skid_asphalt_count_) * sizeof(SkidGpuVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &skid_asphalt_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, skid_asphalt_count_, 1, 0, 0);
  }
  if (skid_rut_count_ > 0) {
    pc.up_dir[3] = 1.0f;
    const VkDescriptorSet sets[2] = {frame_set_, skid_sets_[1]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skid_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, skid_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    VkDeviceSize at = dyn_sync(skid_rut_buf_, VkDeviceSize(skid_rut_count_) * sizeof(SkidGpuVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &skid_rut_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, skid_rut_count_, 1, 0, 0);
  }
}

void Showroom::record_particles_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || (particle_alpha_count_ == 0 && particle_add_count_ == 0)) return;
  struct ParticlePush {
    float viewport_near[4];
    float sun_dir[4];
    float up_dir[4];
  } pc{};
  const uint32_t w = std::max(1u, r_.render_width());
  const uint32_t h = std::max(1u, r_.render_height());
  pc.viewport_near[0] = 1.0f / float(w);
  pc.viewport_near[1] = 1.0f / float(h);
  pc.viewport_near[2] = particle_near_plane_;
  pc.viewport_near[3] = particle_cockpit_ ? 1.85f : 0.0f;
  for (int i = 0; i < 3; ++i) {
    pc.sun_dir[i] = world_sun_[i];
    pc.up_dir[i] = world_up_[i];
  }
  pc.sun_dir[3] = world_up_[3];
  pc.up_dir[3] = (particle_bound_depth_ != VK_NULL_HANDLE) ? 1.0f : 0.0f;

  const VkDescriptorSet sets[2] = {frame_set_, particle_set_};
  const VkDeviceSize at =
      dyn_sync(particle_buf_, VkDeviceSize(particle_alpha_count_ + particle_add_count_) * sizeof(ParticleGpu));
  if (particle_alpha_count_ > 0 && particle_alpha_pipe_) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_alpha_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, particle_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &particle_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, 6, particle_alpha_count_, 0, 0);
  }
  if (particle_add_count_ > 0 && particle_add_pipe_) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_add_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, particle_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &particle_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, 6, particle_add_count_, 0, particle_alpha_count_);
  }
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
    // Skip sky dome, terrain, roads, and large buildings (> 12 m extent).
    const uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
    if ((flags & 2u) != 0) continue;
    const float ex = d.box_max[0] - d.box_min[0];
    const float ey = d.box_max[1] - d.box_min[1];
    const float ez = d.box_max[2] - d.box_min[2];
    if (ex > 12.0f || ey > 12.0f || ez > 12.0f) continue;
    // Closest point on draw AABB to smashed prop sphere.
    const float cx = std::clamp(sx, d.box_min[0], d.box_max[0]);
    const float cy = std::clamp(sy, d.box_min[1], d.box_max[1]);
    const float cz = std::clamp(sz, d.box_min[2], d.box_max[2]);
    const float dx = sx - cx, dy = sy - cy, dz = sz - cz;
    if (dx * dx + dz * dz <= r * r && std::fabs(dy) <= r + 1.5f) {
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
    if (!have(d.texture)) {
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
      auto [it, added] = tmpl_by_tex.emplace(t.texture, uint32_t(rows.size()));
      if (added) {
        gpuworld::DrawGpu row{};
        uint32_t fl = have(t.texture) ? (1u | 32u) : 32u;
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

}  // namespace carview
