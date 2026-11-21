#include "cars/mods.h"
#include "showroom.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <unordered_map>
#include <set>

#include "mask_frag_spv.h"
#include "shadow_frag_spv.h"
#include "showroom_frag_spv.h"
#include "showroom_vert_spv.h"
#include "world_frag_spv.h"
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
                   kFlagCover = 1024;

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
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
// "tirea" (or a car's own "tireatoy_fjcruiser_12") -> "sidewalla_clr.xds".
std::string tyre_texture_member(const std::string& sheet, const std::string& tyre_media) {
  const char letter = tyre_media.size() > 4 ? tyre_media[4] : 'a';
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
  vkDeviceWaitIdle(d);
  stop_reaper();
  if (!guest_) r_.set_scene({});
  for (auto& t : gpu_textures_) r_.destroy_image(t);
  r_.destroy_image(environment_);
  r_.destroy_buffer(vertex_buffer_);
  r_.destroy_buffer(index_buffer_);
  r_.destroy_buffer(frame_buffer_);
  r_.destroy_buffer(shadow_frame_buffer_);
  drop_pending_world();
  drop_fading_world(true);
  r_.destroy_buffer(world_vertices_);
  r_.destroy_buffer(world_indices_);
  for (auto& [record, t] : world_textures_) r_.destroy_image(t);
  for (VkDescriptorPool p : world_pools_) vkDestroyDescriptorPool(d, p, nullptr);
  if (sampler_) vkDestroySampler(d, sampler_, nullptr);
  if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
  for (VkPipeline p : {opaque_, glass_, mask_, world_opaque_, world_coverage_, world_alpha_, shadow_world_,
                       shadow_cutout_, shadow_car_})
    if (p) vkDestroyPipeline(d, p, nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
  if (frame_layout_) vkDestroyDescriptorSetLayout(d, frame_layout_, nullptr);
  if (material_layout_) vkDestroyDescriptorSetLayout(d, material_layout_, nullptr);
}

bool Showroom::load(const std::filesystem::path& bake_dir, const std::string& car,
                    const ShowroomOptions& options, std::string& error) {
  options_ = options;
  std::string err;
  const std::string car_name = lower(car);
  if (!bake::read_file(cars::mods::resolve(bake_dir / "cars" / (car_name + ".shiftbake")), car_, &err)) {
    error = car_name + ": " + err;
    return false;
  }
  const bake::CarInfo& info = car_.car;
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
  if (!bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (info.wheel_media + ".shiftbake")), wheel_, &err)) {
    error = "wheel " + info.wheel_media + ": " + err;
    return false;
  }
  if (!bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (info.tyre_media + ".shiftbake")), tyre_, &err) ||
      !bake::read_file(cars::mods::resolve(bake_dir / "wheels" / (info.tyre_texture_media + ".shiftbake")), tyre_textures_,
                       &err) ||
      !bake::read_file(cars::mods::resolve(bake_dir / "shared.shiftbake"), shared_, &err)) {
    error = err;
    return false;
  }

  textures_.push_back({nullptr, "@white"});
  textures_.push_back({nullptr, "@black"});

  // The row shown: the requested car id, else the baked row.
  stock_ = nullptr;
  for (const auto& s : info.stock_rows)
    if (s.known && (options_.car_id == 0 ? s.car_id == info.car_id : s.car_id == options_.car_id)) stock_ = &s;
  if (options_.car_id != 0 && !stock_)
    notes_.push_back("no stock row for car " + std::to_string(options_.car_id) + " in the bake");
  if (!stock_) notes_.push_back("no stock parts baked: hiding variants by letter (first real letter)");
  // FORMAT-CARS, Stock parts: family members by the stock rows, seats a
  // stock weight row removes, and other lettered names by the first real
  // letter.
  auto hidden = [&](const std::string& name, const std::set<std::string>& real) {
    if (options_.all_variants) return false;
    const std::string n = lower(name);
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
      // A stock family member is filled from its LOD1-5 twin; another
      // member never stands in for it (no promotion).
      const bool by_stock = stock_ && !family_member(n).empty();
      if (by_stock && hidden(n, real)) continue;
      if (!by_stock && ends_with(n, "race")) continue;
      if (!by_stock && stock_ && hidden(n, real)) continue;  // a removed seat
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

  // Brakes: each corner's rotor and calliper, centred on the hub.
  static constexpr bake::Corner kCorners[4] = {bake::Corner::kLF, bake::Corner::kRF, bake::Corner::kLR,
                                               bake::Corner::kRR};
  for (const auto& m : car_.models) {
    if (m.role != Role::kCaliper && m.role != Role::kRotor) continue;
    int corner = -1;
    for (int c = 0; c < 4; ++c)
      if (m.corner == kCorners[c]) corner = c;
    if (corner < 0) {
      notes_.push_back(m.name + ": no corner");
      continue;
    }
    float m4[16];
    translate_scale(info.wheel_centres[size_t(corner)], {1, 1, 1}, m4);
    std::vector<std::string> parts;
    for (const auto& p : m.parts) parts.push_back(p.name);
    // For set_wheel_poses: the brake model about its hub in the body frame
    // is the pose's linear part (the models are authored in the model frame).
    pending_wheel_ = corner;
    pending_spins_ = m.role == Role::kRotor;
    std::memcpy(pending_local_, pose, sizeof pose);
    pending_local_[13] = pending_local_[14] = 0;
    add_model(m, parts, m4, car_, corner < 2 ? 0 : 1);
  }
  pending_wheel_ = -1;
  if (info.caliper_angle[0] != 0 || info.caliper_angle[1] != 0)
    notes_.push_back("calliper angles not applied (spec question)");

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
    // Body-frame wheel matrix: centre, the half turn for right wheels, then
    // the lateral scale (and the tyre's lateral offset in the rim's x).
    auto wheel_matrix = [&](float sx, float dx, float* m4) {
      float local[16];
      identity(local);
      const float turn = right ? -1.0f : 1.0f;
      local[0] = turn * sx;
      local[10] = turn;
      const auto& cb = info.wheel_centres_body[size_t(c)];
      local[12] = turn * dx;
      std::memcpy(pending_local_, local, sizeof local);  // about the centre (set_wheel_poses)
      pending_wheel_ = c;
      pending_spins_ = true;
      local[12] += cb[0];
      local[13] = cb[1];
      local[14] = cb[2];
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
      std::memcpy(d.push.model, transform, sizeof d.push.model);
      d.wheel = pending_wheel_;
      d.spins = pending_spins_;
      std::memcpy(d.local, pending_local_, sizeof d.local);
      std::memcpy(d.push.uv0, &s.uv_transform[0], 4 * sizeof(float));
      std::memcpy(d.push.uv1, &s.uv_transform[4], 4 * sizeof(float));
      uint32_t flags = 0;
      // Glass techniques never sample the body atlas (FORMAT-CARS, Paint):
      // an opaque glass draw (the windscreen's black_glass band) is a flat
      // dark grey, the shade of the glass draws' constant not being known.
      const bool flat_glass = s.material != MaterialClass::kGlass && s.slots[0] == "@atlas" &&
                              s.technique.find("glass") != std::string::npos;
      const uint32_t t0 = flat_glass ? UINT32_MAX : texture_index(s.slots[0], pack, s.lod == 0);
      const uint32_t t1 = texture_index(s.slots[1], pack, s.lod == 0);
      if (t0 == UINT32_MAX) flags |= kFlagNoSlot0;
      if (t1 != UINT32_MAX) flags |= kFlagSlot1;
      if (s.material == MaterialClass::kPaint) {
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
      } else if (s.material == MaterialClass::kCaliper) {
        const uint32_t rgb = info.caliper_rgb[size_t(axle)];
        d.push.tint[0] = float((rgb >> 16) & 0xFF) / 255.0f;
        d.push.tint[1] = float((rgb >> 8) & 0xFF) / 255.0f;
        d.push.tint[2] = float(rgb & 0xFF) / 255.0f;
      }
      // A grille shows its Shared tile (slot 1) on the tiled UV0; the
      // alpha-tested techniques (FORMAT-CARS: grille1_alpha) cut it out.
      const std::string technique = lower(s.technique);
      if (t1 != UINT32_MAX && technique.rfind("grille", 0) == 0) {
        flags |= kFlagGrille;
        if (ends_with(technique, "_alpha")) flags |= kFlagCutout;
      }
      if (flat_glass) {
        flags |= kFlagFlat;
        d.push.tint[0] = d.push.tint[1] = d.push.tint[2] = 0.02f;
      }
      // The mirror glass (mirrorLeft, mirrorRight, mirrorMiddle): no
      // reflection yet, a flat dark grey (interim, ours) rather than the
      // atlas corner it maps to.
      if (technique.rfind("mirror", 0) == 0) {
        flags |= kFlagFlat;
        d.push.tint[0] = 0.05f, d.push.tint[1] = 0.055f, d.push.tint[2] = 0.06f;
      }
      // Lamp covers (detail_glass_red, _amber, _clear): coloured glass by
      // the technique's name, not the cabin glass's dark tint (an interim:
      // the glass constants are not known).
      bool red_cover = false;
      if (s.material == MaterialClass::kGlass && technique.rfind("detail_glass_", 0) == 0) {
        const std::string hue = technique.substr(13);
        float c[3] = {0.55f, 0.58f, 0.6f};
        if (hue.rfind("red", 0) == 0) c[0] = 0.22f, c[1] = 0.006f, c[2] = 0.005f, red_cover = true;
        else if (hue.rfind("amber", 0) == 0) c[0] = 0.6f, c[1] = 0.22f, c[2] = 0.01f;
        flags |= kFlagCover;
        std::memcpy(d.push.tint, c, sizeof c);
      }
      // The brake lamps: the red covers (the R34's sit on glassLTL,
      // glassRTL and the trunk's high stop) and the tail-lamp techniques.
      d.brake_lamp = red_cover || (s.material == MaterialClass::kLamp && (technique.rfind("taillight", 0) == 0 ||
                                                                         technique.rfind("tail_light", 0) == 0));
      d.push.tint[3] = 1;
      // Numbered copies of the interior techniques (the R34's bump_plastic2,
      // bump_leather2) bake as opaque; they are cabin surfaces (FORMAT-CARS'
      // interior list by name; interim, ours).
      MaterialClass klass = s.material;
      if (klass == MaterialClass::kOpaque) {
        std::string base_name = technique;
        while (!base_name.empty() && std::isdigit(static_cast<unsigned char>(base_name.back()))) base_name.pop_back();
        static const char* const kCabin[] = {"leather", "cloth", "plastic", "solid", "mottled", "headliner",
                                             "bump_leather", "bump_plastic", "bump_stitching"};
        if (base_name != technique)
          for (const char* c : kCabin)
            if (base_name == c) klass = MaterialClass::kInterior;
      }
      // The cabin techniques' texel scale (CARS, Cockpit interior and
      // mirrors: the effect defaults, then the car's lighting): plastic,
      // cloth and solid 0.5, mottled 0.35, bump_plastic 0.3, gauges 1.
      // Leather, headliner and the other bump techniques: 0.5 (ours).
      if (klass == MaterialClass::kInterior) {
        std::string base_name = technique;
        while (!base_name.empty() && std::isdigit(static_cast<unsigned char>(base_name.back()))) base_name.pop_back();
        const float k = base_name == "mottled" ? 0.35f : base_name == "bump_plastic" ? 0.3f : 0.5f;
        d.push.tint[0] = d.push.tint[1] = d.push.tint[2] = k;
      }
      d.push.params[0] = float(uint32_t(klass));
      d.push.params[1] = float(flags);
      d.textures = {t0 == UINT32_MAX ? 0u : t0, t1 == UINT32_MAX ? 0u : t1,
                    livery == UINT32_MAX ? 1u : livery};
      d.glass = s.material == MaterialClass::kGlass;
      draws_.push_back(d);
      drawn = true;
    }
    if (drawn) drawn_parts_.push_back(model.name + "/" + part.name);
  }
}

void Showroom::set_braking(float glow) {
  for (Draw& d : draws_)
    if (d.brake_lamp) d.push.params[2] = glow;
}

void Showroom::set_wheel_poses(const WheelPose poses[4]) {
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
  }
}

// The levels of `texture` that form a mip chain with valid sizes, and
// their bytes.
static uint32_t texture_chain(const bake::Texture& texture, VkDeviceSize& total) {
  const uint32_t w0 = texture.levels[0].width, h0 = texture.levels[0].height;
  uint32_t levels = 0;
  total = 0;
  for (const auto& level : texture.levels) {
    const uint32_t w = std::max(1u, w0 >> levels), h = std::max(1u, h0 >> levels);
    if (level.width != w || level.height != h || level.data.size() != level_bytes(texture.format, w, h)) break;
    total += level.data.size();
    ++levels;
  }
  return levels;
}

bool Showroom::create_texture_image(const bake::Texture& texture, bool srgb, uint32_t levels, GpuTexture& out,
                                    std::string& error) {
  const VkFormat format = vk_format(texture.format, srgb);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {texture.levels[0].width, texture.levels[0].height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VK_TRY(vkCreateImage(r_.device_, &info, nullptr, &out.image), "vkCreateImage");
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(r_.device_, out.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = r_.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_TRY(vkAllocateMemory(r_.device_, &alloc, nullptr, &out.memory), "vkAllocateMemory");
  vkBindImageMemory(r_.device_, out.image, out.memory, 0);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.components = {vk_swizzle(texture.swizzle[0]), vk_swizzle(texture.swizzle[1]),
                     vk_swizzle(texture.swizzle[2]), vk_swizzle(texture.swizzle[3])};
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  VK_TRY(vkCreateImageView(r_.device_, &view, nullptr, &out.view), "vkCreateImageView");
  return true;
}

// Copies `levels` levels, packed from `offset` in `staging`, into `image`
// and leaves it ready to sample.
static void record_texture_copy(VkCommandBuffer cmd, VkBuffer staging, VkDeviceSize offset, const bake::Texture& texture,
                         uint32_t levels, VkImage image) {
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t i = 0; i < levels; ++i) {
    const auto& level = texture.levels[i];
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
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
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
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
  VkDeviceSize total = 0;
  const uint32_t levels = texture_chain(*texture, total);
  if (!levels) {
    error = "texture " + texture->name + ": level 0 has the wrong size";
    return false;
  }
  if (!create_texture_image(*texture, srgb, levels, out, error)) return false;
  Renderer::Buffer staging;
  if (!r_.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        staging)) {
    error = "texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  vkMapMemory(r_.device_, staging.memory, 0, total, 0, reinterpret_cast<void**>(&mapped));
  VkDeviceSize offset = 0;
  for (uint32_t i = 0; i < levels; ++i) {
    std::memcpy(mapped + offset, texture->levels[i].data.data(), texture->levels[i].data.size());
    offset += texture->levels[i].data.size();
  }
  vkUnmapMemory(r_.device_, staging.memory);
  const bool ok = r_.submit_once(
      [&](VkCommandBuffer cmd) { record_texture_copy(cmd, staging.buffer, 0, *texture, levels, out.image); });
  r_.destroy_buffer(staging);
  if (!ok) {
    error = "texture upload";
    return false;
  }
  return true;
}

bool Showroom::upload_environment(std::string& error) {
  constexpr uint32_t kSize = 64, kLevels = 7;
  // Level 0 from the studio function, then 2x2 box filters per face.
  std::vector<std::vector<std::array<float, 3>>> faces[kLevels];
  for (uint32_t face = 0; face < 6; ++face) {
    std::vector<std::array<float, 3>> texels(kSize * kSize);
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
        texels[y * kSize + x] = studio(d[0], d[1], d[2]);
      }
    faces[0].push_back(std::move(texels));
  }
  for (uint32_t level = 1; level < kLevels; ++level) {
    const uint32_t s = kSize >> level, p = s * 2;
    for (uint32_t face = 0; face < 6; ++face) {
      const auto& prev = faces[level - 1][face];
      std::vector<std::array<float, 3>> texels(s * s);
      for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x)
          for (int c = 0; c < 3; ++c)
            texels[y * s + x][size_t(c)] =
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
  VK_TRY(vkCreateImage(r_.device_, &info, nullptr, &environment_.image), "vkCreateImage (cube)");
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(r_.device_, environment_.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = r_.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_TRY(vkAllocateMemory(r_.device_, &alloc, nullptr, &environment_.memory), "vkAllocateMemory");
  vkBindImageMemory(r_.device_, environment_.image, environment_.memory, 0);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = environment_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &view, nullptr, &environment_.view), "vkCreateImageView (cube)");
  Renderer::Buffer staging;
  const VkDeviceSize bytes = halfs.size() * 2;
  if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        staging)) {
    error = "environment staging buffer";
    return false;
  }
  void* mapped = nullptr;
  vkMapMemory(r_.device_, staging.memory, 0, bytes, 0, &mapped);
  std::memcpy(mapped, halfs.data(), size_t(bytes));
  vkUnmapMemory(r_.device_, staging.memory);
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
  // Set 0: the frame block, the environment cube, the sun shadow map.
  const VkDescriptorSetLayoutBinding frame_bindings[3] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 3;
  set.pBindings = frame_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &frame_layout_), "frame set layout");
  VkDescriptorSetLayoutBinding material_bindings[3];
  for (uint32_t i = 0; i < 3; ++i)
    material_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr};
  set.bindingCount = 3;
  set.pBindings = material_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &material_layout_),
         "material set layout");
  const VkDescriptorSetLayout layouts[2] = {frame_layout_, material_layout_};
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 2;
  layout.pSetLayouts = layouts;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &layout, nullptr, &pipeline_layout_), "pipeline layout");

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(r_.device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, mask = VK_NULL_HANDLE;
  VK_TRY(make_module(kShowroom_vert_spv, sizeof(kShowroom_vert_spv), vert), "showroom.vert");
  VK_TRY(make_module(kShowroom_frag_spv, sizeof(kShowroom_frag_spv), frag), "showroom.frag");
  VK_TRY(make_module(kMask_frag_spv, sizeof(kMask_frag_spv), mask), "mask.frag");

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
  auto build = [&](VkShaderModule fragment, VkFormat color_format, VkSampleCountFlagBits samples,
                   bool depth_attachment, bool depth_write, bool blend_on, VkPipeline& out,
                   VkShaderModule vertex = VK_NULL_HANDLE, const VkPipelineVertexInputStateCreateInfo* input = nullptr,
                   bool coverage = false) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vertex ? vertex : vert, "main", vertex ? &spec : nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         fragment, "main", fragment == mask ? nullptr : &spec}};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    multisample.alphaToCoverageEnable = coverage && samples != VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth_attachment;
    depth.depthWriteEnable = depth_write;
    depth.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend_on) {
      // Premultiplied colour over what is behind.
      attachment.blendEnable = VK_TRUE;
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      attachment.colorBlendOp = VK_BLEND_OP_ADD;
      attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = depth_attachment ? r_.depth_format_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = input ? input : &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = vertex ? &raster_bias : &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = vertex ? &dynamic_bias : &dynamic;
    info.layout = pipeline_layout_;
    return vkCreateGraphicsPipelines(r_.device_, VK_NULL_HANDLE, 1, &info, nullptr, &out);
  };
  VkResult result = build(frag, r_.scene_format(), r_.samples_, true, true, false, opaque_);
  if (result == VK_SUCCESS) result = build(frag, r_.scene_format(), r_.samples_, true, false, true, glass_);
  if (result == VK_SUCCESS)
    result = build(mask, VK_FORMAT_R8_UNORM, r_.samples_, false, false, false, mask_);
  // The textured world (world.vert / world.frag): opaque, opaque with alpha
  // to coverage, and blended with depth writes off.
  VkShaderModule world_vert = VK_NULL_HANDLE, world_frag = VK_NULL_HANDLE;
  if (result == VK_SUCCESS) result = make_module(kWorld_vert_spv, sizeof(kWorld_vert_spv), world_vert);
  if (result == VK_SUCCESS) result = make_module(kWorld_frag_spv, sizeof(kWorld_frag_spv), world_frag);
  VkVertexInputBindingDescription world_binding{0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription world_attributes[6] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
      {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(WorldVertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv0)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv1)},
      {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldVertex, colour)},
      {5, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(WorldVertex, centre)}};
  VkPipelineVertexInputStateCreateInfo world_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  world_input.vertexBindingDescriptionCount = 1;
  world_input.pVertexBindingDescriptions = &world_binding;
  world_input.vertexAttributeDescriptionCount = 6;
  world_input.pVertexAttributeDescriptions = world_attributes;
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, true, false, world_opaque_, world_vert,
                   &world_input);
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, true, false, world_coverage_, world_vert,
                   &world_input, true);
  if (result == VK_SUCCESS)
    result = build(world_frag, r_.scene_format(), r_.samples_, true, false, true, world_alpha_, world_vert,
                   &world_input);
  // The sun shadow pass (ours): depth only into the shadow map, biased;
  // opaque casters with no fragment shader, cut-outs alpha-tested.
  VkShaderModule shadow_frag = VK_NULL_HANDLE;
  auto build_shadow = [&](VkShaderModule vertex, VkShaderModule fragment,
                          const VkPipelineVertexInputStateCreateInfo* input, VkPipeline& out) {
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
    info.layout = pipeline_layout_;
    return vkCreateGraphicsPipelines(r_.device_, VK_NULL_HANDLE, 1, &info, nullptr, &out);
  };
  if (result == VK_SUCCESS && r_.options_.shadow.size > 0) {
    result = make_module(kShadow_frag_spv, sizeof(kShadow_frag_spv), shadow_frag);
    if (result == VK_SUCCESS) result = build_shadow(world_vert, VK_NULL_HANDLE, &world_input, shadow_world_);
    if (result == VK_SUCCESS) result = build_shadow(world_vert, shadow_frag, &world_input, shadow_cutout_);
    if (result == VK_SUCCESS) result = build_shadow(vert, VK_NULL_HANDLE, &vertex_input, shadow_car_);
  }
  if (shadow_frag) vkDestroyShaderModule(r_.device_, shadow_frag, nullptr);
  if (world_vert) vkDestroyShaderModule(r_.device_, world_vert, nullptr);
  if (world_frag) vkDestroyShaderModule(r_.device_, world_frag, nullptr);
  vkDestroyShaderModule(r_.device_, vert, nullptr);
  vkDestroyShaderModule(r_.device_, frag, nullptr);
  vkDestroyShaderModule(r_.device_, mask, nullptr);
  VK_TRY(result, "showroom pipelines");
  return true;
}

bool Showroom::upload(std::string& error) {
  if (!r_.ensure_shadow_map(error) || !create_pipelines(error)) return false;
  const bool shadows = r_.options_.shadow.size > 0;
  if (shadows && !r_.create_buffer(sizeof(ShowroomFrame),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shadow_frame_buffer_)) {
    error = "shadow frame buffer";
    return false;
  }
  if (!r_.upload_buffer(vertices_.data(), vertices_.size() * sizeof(bake::Vertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_) ||
      !r_.upload_buffer(indices_.data(), indices_.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        index_buffer_) ||
      !r_.create_buffer(sizeof(ShowroomFrame),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame_buffer_)) {
    error = "showroom buffers";
    return false;
  }
  gpu_textures_.resize(textures_.size());
  for (size_t i = 0; i < textures_.size(); ++i) {
    const auto* t = textures_[i].texture;
    bool srgb = true;
    if (t && (t->format == bake::TextureFormat::kBc4 || t->format == bake::TextureFormat::kBc5 ||
              t->format == bake::TextureFormat::kR8))
      srgb = false;
    if (i == 1) {
      // Built-in black (no livery).
      bake::Texture black;
      black.name = "@black";
      black.levels.push_back({1, 1, {0, 0, 0, 255}});
      if (!upload_texture(&black, false, gpu_textures_[i], error)) return false;
      continue;
    }
    if (!upload_texture(t, srgb, gpu_textures_[i], error)) {
      // A texture we cannot use: fall back to white and note it.
      notes_.push_back(error);
      error.clear();
      r_.destroy_image(gpu_textures_[i]);
      if (!upload_texture(nullptr, true, gpu_textures_[i], error)) return false;
    }
  }
  if (!upload_environment(error)) return false;

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

  // One material set per distinct (slot 0, slot 1, livery).
  for (auto& d : draws_) {
    auto [it, inserted] = descriptor_by_textures_.emplace(d.textures, uint32_t(descriptor_by_textures_.size()));
    d.descriptor = it->second;
  }
  const uint32_t sets = uint32_t(descriptor_by_textures_.size());
  const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 3 + 4}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = sets + 2;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = sizes;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &frame_layout_;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &frame_set_), "frame set");
  if (shadows) VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &shadow_set_), "shadow frame set");
  // The frame set, and the shadow pass's (its own frame block; the map at
  // binding 2 is never read by the pass's shaders).
  for (VkDescriptorSet target : {frame_set_, shadow_set_}) {
    if (!target) continue;
    VkDescriptorBufferInfo buffer{target == frame_set_ ? frame_buffer_.buffer : shadow_frame_buffer_.buffer, 0,
                                  sizeof(ShowroomFrame)};
    VkDescriptorImageInfo cube{sampler_, environment_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map{r_.shadow_sampler_, r_.shadow_map_.view,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet = target;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &buffer;
    writes[1].dstSet = target;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &cube;
    writes[2].dstSet = target;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &map;
    vkUpdateDescriptorSets(r_.device_, 3, writes, 0, nullptr);
  }
  material_sets_.resize(sets);
  std::vector<VkDescriptorSetLayout> layouts(sets, material_layout_);
  alloc.descriptorSetCount = sets;
  alloc.pSetLayouts = layouts.data();
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, material_sets_.data()), "material sets");
  for (const auto& [textures, index] : descriptor_by_textures_) {
    VkDescriptorImageInfo images[3];
    VkWriteDescriptorSet writes[3];
    for (uint32_t b = 0; b < 3; ++b) {
      images[b] = {sampler_, gpu_textures_[textures[b]].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[b].dstSet = material_sets_[index];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(r_.device_, 3, writes, 0, nullptr);
  }

  if (guest_) return true;
  SceneHooks hooks;
  hooks.before = [this](VkCommandBuffer cmd) {
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
    const bool shadows = shadows_on();
    if (shadows) {
      place_shadow();
    } else {
      std::memset(frame_.shadow_matrix, 0, sizeof frame_.shadow_matrix);
      std::memset(frame_.shadow, 0, sizeof frame_.shadow);
      std::memset(frame_.shadow_bias, 0, sizeof frame_.shadow_bias);
      shadow_drawn_ = shadow_cars_ = 0;
    }
    update_frame(cmd);
    // Each guest's frame block: this car's camera and light, its own stripe.
    std::vector<Showroom*> seen;
    for (const auto& in : instances_) {
      if (!in.car || std::find(seen.begin(), seen.end(), in.car) != seen.end()) continue;
      seen.push_back(in.car);
      in.car->frame_ = frame_;
      for (int i = 0; i < 4; ++i) in.car->frame_.stripe[i] = in.car->stripe_[i];
      in.car->update_frame(cmd);
      if (shadows && in.car->shadow_set_) {
        in.car->shadow_frame_ = shadow_frame_;
        in.car->update_buffer(cmd, in.car->shadow_frame_buffer_.buffer, in.car->shadow_frame_);
      }
    }
    if (shadows) {
      update_buffer(cmd, shadow_frame_buffer_.buffer, shadow_frame_);
      record_shadow(cmd);
      r_.mark(cmd, "shadows");
    }
  };
  hooks.draw = [this](VkCommandBuffer cmd) {
    r_.mark(cmd, "setup");
    record_world(cmd);
    if (draw_car_) record_draws(cmd, opaque_, glass_);
    for (const auto& in : instances_)
      if (in.car) in.car->record_instance(cmd, in.model);
    r_.mark(cmd, "car");
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

void Showroom::update_frame(VkCommandBuffer cmd) { update_buffer(cmd, frame_buffer_.buffer, frame_); }

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
  vkCmdUpdateBuffer(cmd, buffer, 0, sizeof(ShowroomFrame), &f);
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

void Showroom::record_draws(VkCommandBuffer cmd, VkPipeline opaque, VkPipeline glass) {
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  auto draw = [&](const Draw& d) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                            &material_sets_[d.descriptor], 0, nullptr);
    const ShowroomPush& push = d.push;
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  };
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
  for (const auto& d : draws_)
    if (!d.glass || opaque == glass) draw(d);
  if (opaque == glass) return;
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
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glass);
  for (const Draw* d : sorted) draw(*d);
}

bool Showroom::shadows_on() const {
  return shadow_set_ && shadow_world_ && r_.shadow_size_ > 1 && world_textured_ && !world_draws_.empty();
}

void Showroom::place_shadow() {
  const ShadowSettings& s = r_.options_.shadow;
  const float radius = s.radius;
  // Depth: 400 m either side of the box's centre along the sun (ours).
  constexpr float kDepth = 400.0f;
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
  // The box's centre: half its radius ahead of the eye along the view (the
  // projection's w row), flattened onto the ground plane.
  const float* vp = frame_.view_projection;
  float ahead[3] = {vp[3], vp[7], vp[11]};
  const float along_up = dot3(ahead, up);
  for (int k = 0; k < 3; ++k) ahead[k] -= along_up * up[k];
  if (!normalise(ahead)) ahead[0] = 0, ahead[1] = 0, ahead[2] = 1;
  float centre[3];
  for (int k = 0; k < 3; ++k) centre[k] = frame_.eye[k] + ahead[k] * radius * 0.5f;
  // The sun's view: x and y across it, depth along it.
  const float forward[3] = {-sun[0], -sun[1], -sun[2]};
  float right[3], across[3];
  cross(forward, up, right);
  if (!normalise(right)) {
    const float x[3] = {1, 0, 0};
    cross(forward, x, right);
    normalise(right);
  }
  cross(right, forward, across);
  // Snapped to whole texels measured from the scenery's origin, so the map
  // does not crawl as the car moves.
  const float texel = 2.0f * radius / float(r_.shadow_size_);
  const float origin[3] = {world_transform_[12], world_transform_[13], world_transform_[14]};
  float cx = dot3(centre, right), cy = dot3(centre, across);
  const float ox = dot3(origin, right), oy = dot3(origin, across);
  cx = ox + std::round((cx - ox) / texel) * texel;
  cy = oy + std::round((cy - oy) / texel) * texel;
  const float cz = dot3(centre, sun);
  float m[16] = {};
  for (int k = 0; k < 3; ++k) {
    m[k * 4 + 0] = right[k] / radius;
    m[k * 4 + 1] = across[k] / radius;
    m[k * 4 + 2] = -sun[k] / (2.0f * kDepth);
  }
  m[12] = -cx / radius;
  m[13] = -cy / radius;
  m[14] = 0.5f + cz / (2.0f * kDepth);
  m[15] = 1.0f;
  std::memcpy(frame_.shadow_matrix, m, sizeof m);
  frame_.shadow[0] = 1.0f;
  frame_.shadow[1] = 1.0f / float(r_.shadow_size_);
  frame_.shadow[2] = s.strength;
  frame_.shadow[3] = float(s.taps);
  frame_.shadow_bias[0] = 1.5f * texel;
  frame_.shadow_bias[1] = frame_.shadow_bias[2] = frame_.shadow_bias[3] = 0.0f;
  shadow_frame_ = frame_;
  std::memcpy(shadow_frame_.view_projection, m, sizeof m);
  std::memcpy(shadow_frame_.transparent_view_projection, m, sizeof m);
  // Tree sprites turn to the eye: here to the sun, far up its ray.
  for (int k = 0; k < 3; ++k) shadow_frame_.eye[k] = centre[k] + sun[k] * 1000.0f;
  shadow_frame_.eye[3] = 1.0f;
  // No per-instance level pick from the sun's eye (world.vert): every level
  // casts, as before the pick.
  shadow_frame_.world_fade[1] = 0.0f;
}

void Showroom::record_shadow(VkCommandBuffer cmd) {
  const uint32_t size = r_.shadow_size_;
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
  rendering.renderArea = {{0, 0}, {size, size}};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);
  const VkViewport viewport{0, 0, float(size), float(size), 0, 1};
  const VkRect2D scissor{{0, 0}, {size, size}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  // The scenery's casters, culled against the map's box (its six planes in
  // the scenery's frame, as record_world culls against the view).
  const float* vp = shadow_frame_.view_projection;
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
  shadow_drawn_ = shadow_cars_ = 0;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &world_vertices_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, world_indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &shadow_set_, 0, nullptr);
  VkPipeline bound = VK_NULL_HANDLE;
  VkDescriptorSet bound_set = VK_NULL_HANDLE;
  const auto eye = eye_in_scenery(world_transform_);
  for (const auto& d : world_draws_) {
    // Blended draws (decals, the lake) and the uncullable sky cast none.
    if (!d.cull || d.blend >= 2 || outside(d) || near_lod_beyond(d, eye)) continue;  // blended and additive draws cast nothing
    ++shadow_drawn_;
    const VkPipeline pipeline = d.blend == 1 ? shadow_cutout_ : shadow_world_;
    if (pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
    ShowroomPush push = d.push;
    if (d.blend == 1) {
      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      if (!set) {
        set = world_white_set_;
        push.params[1] = 0;
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &(bound_set = set), 0,
                                nullptr);
    }
    std::memcpy(push.model, world_transform_, sizeof push.model);
    std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
    std::memcpy(push.uv1, world_up_, sizeof push.uv1);
    push.params[2] = 0;  // no far-ring depth push from the sun's eye
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
  }
  // The car, and the traffic whose origin is within the box (5 m spare).
  if (draw_car_) record_shadow_car(cmd, nullptr);
  const float reach = 1.0f + 5.0f / r_.options_.shadow.radius;
  for (const auto& in : instances_) {
    if (!in.car) continue;
    const float* t = in.model + 12;
    const float x = vp[0] * t[0] + vp[4] * t[1] + vp[8] * t[2] + vp[12];
    const float y = vp[1] * t[0] + vp[5] * t[1] + vp[9] * t[2] + vp[13];
    if (std::fabs(x) > reach || std::fabs(y) > reach) continue;
    in.car->record_shadow_car(cmd, in.model);
    ++shadow_cars_;
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

void Showroom::record_shadow_car(VkCommandBuffer cmd, const float* m) {
  if (draws_.empty() || !shadow_car_ || !shadow_set_) return;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_car_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &shadow_set_, 0, nullptr);
  for (const auto& d : draws_) {
    if (d.glass) continue;  // glass and lamp covers cast none (ours)
    ShowroomPush push = d.push;
    if (m) multiply(m, d.push.model, push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  }
}

void Showroom::record_instance(VkCommandBuffer cmd, const float m[16]) {
  if (draws_.empty() || material_sets_.empty()) return;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  for (int pass = 0; pass < 2; ++pass) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass ? glass_ : opaque_);
    for (const auto& d : draws_) {
      if (d.glass != (pass == 1)) continue;
      ShowroomPush push = d.push;
      multiply(m, d.push.model, push.model);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                              &material_sets_[d.descriptor], 0, nullptr);
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
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
    for (size_t j = first; j < last; ++j) {
      const uint32_t t = cells[j].second;
      for (int k = 0; k < 3; ++k) {
        const size_t c = size_t(t) * 3 + size_t(k);
        VertexKey key{};
        WorldVertex& v = key.v;
        for (int i = 0; i < 3; ++i) v.position[i] = world.positions[c][size_t(i)];
        for (int i = 0; i < 3; ++i) v.normal[i] = snorm8(world.normals[c][size_t(i)]);
        for (int i = 0; i < 2; ++i) v.uv0[i] = world.uv0[c][size_t(i)], v.uv1[i] = world.uv1[c][size_t(i)];
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
    if (flags && mat.layers == 3) flags |= 16;
    if (flags && mat.layers == 4) flags |= 32;  // tree sprite: turned to the camera in world.vert
    if (flags) std::memcpy(d.push.tint, mat.tiling.data(), sizeof d.push.tint);
    // The distance cross-fade (world.frag): 128 a prop's LOD00, 256 its
    // LOD02, 512 a prop of one level.
    if (mat.fade >= 1 && mat.fade <= 3) flags |= 64u << mat.fade;
    d.push.params[1] = float(flags);
    d.push.params[2] = float(mat.far);                                 // world.vert's depth push
    // Light gain (world.frag): WORLD-MATERIALS has something brighten the
    // vertex-blended hills that its shading does not show, and says to
    // scale them to the reference; 2.5 brings their median near the
    // frame's (ours).
    d.push.params[3] = (flags & 4) ? 2.5f : 1.0f;
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
      float nv[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      const float len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
      if (!(len > 0)) continue;
      for (float& k : nv) k /= len;
      if (nv[1] < 0)
        for (float& k : nv) k = -k;  // up-facing; the shader faces it to the eye anyway
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
      for (const auto* p : {&a, &b, &c}) {
        v.position = *p;
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
  vkDeviceWaitIdle(r_.device_);
  drop_fading_world(true);
  r_.destroy_buffer(world_vertices_);
  r_.destroy_buffer(world_indices_);
  world_draws_.clear();
  world_triangles_ = 0;
  if (geometry.indices.empty()) return true;
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
  return true;
}

bool Showroom::stage_world(WorldGeometry geometry, StagedWorld& out, std::string& error) {
  out = StagedWorld{};
  out.draws = std::move(geometry.draws);
  out.triangles = geometry.triangles;
  out.textured = geometry.textured;
  out.textures = std::move(geometry.textures);
  if (geometry.indices.empty()) return true;
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
    if (vkMapMemory(r.device_, buffer->memory, 0, size, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, data, size_t(size));
    vkUnmapMemory(r.device_, buffer->memory);
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
  // The new textures' levels, packed (each from a 16-byte boundary).
  VkDeviceSize texel_bytes = 0;
  out.texel_ranges.assign(out.textures.size(), {0, 0});
  for (size_t i = 0; i < out.textures.size(); ++i) {
    const auto& t = out.textures[i];
    if (!t || t->texture.levels.empty()) continue;
    VkDeviceSize bytes = 0;
    const uint32_t levels = texture_chain(t->texture, bytes);
    if (!levels) continue;  // uploaded apart, which reports it
    out.texel_ranges[i] = {texel_bytes, levels};
    texel_bytes += (bytes + 15) / 16 * 16;
  }
  if (texel_bytes) {
    std::vector<uint8_t> packed(size_t(texel_bytes), 0);
    for (size_t i = 0; i < out.textures.size(); ++i) {
      if (!out.texel_ranges[i].second) continue;
      size_t at = size_t(out.texel_ranges[i].first);
      for (uint32_t l = 0; l < out.texel_ranges[i].second; ++l) {
        const auto& data = out.textures[i]->texture.levels[l].data;
        std::memcpy(packed.data() + at, data.data(), data.size());
        at += data.size();
      }
    }
    if (!stage(packed.data(), texel_bytes, out.texels)) {
      out.texels.reset();  // every texture uploads apart instead
      out.texel_ranges.assign(out.textures.size(), {0, 0});
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
      if (!create_texture_image(t->texture, t->srgb, out.texel_ranges[i].second, (*out.images)[i], why))
        r.destroy_image((*out.images)[i]);  // begin_world tries again and reports it
    }
  }
  return true;
}

bool Showroom::begin_world_texture(const WorldTextureData& t, VkBuffer texels, VkDeviceSize offset, uint32_t levels,
                                   VkCommandBuffer cmd, Renderer::Image* made, std::string& error) {
  GpuTexture image;
  if (made && made->image) {
    image = *made;
    *made = {};
  } else if (!create_texture_image(t.texture, t.srgb, levels, image, error)) {
    r_.destroy_image(image);
    return false;
  }
  record_texture_copy(cmd, texels, offset, t.texture, levels, image.image);
  {
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    world_textures_[t.record] = image;
  }
  // Its set: the texture at binding 0, white at 1 and 2. Drawn only once
  // the scenery's copy is fenced (poll_world).
  const VkDescriptorSet set = world_layer_set({t.record, kNoWorldTexture, kNoWorldTexture});
  if (!set) {
    error = "world texture set";
    return false;
  }
  world_sets_[t.record] = set;
  return true;
}

void Showroom::drop_pending_world() {
  if (!pending_world_) return;
  auto& p = *pending_world_;
  if (p.fence) {
    vkWaitForFences(r_.device_, 1, &p.fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(r_.device_, p.fence, nullptr);
  }
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
}

void Showroom::stop_reaper() {
  // The GPU is idle (the caller waited): what is retired can go now.
  for (auto& [tag, f] : retired_) f();
  retired_.clear();
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
    if (!r_.create_buffer(vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p->vertices) ||
        !r_.create_buffer(ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p->indices) ||
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
    const VkBufferCopy v{0, 0, vb}, i{0, 0, ib};
    vkCmdCopyBuffer(p->cmd, p->staged.vertices->buffer, p->vertices.buffer, 1, &v);
    vkCmdCopyBuffer(p->cmd, p->staged.indices->buffer, p->indices.buffer, 1, &i);
    if (fenced) {
      const auto& st = p->staged;
      for (size_t k = 0; k < st.textures.size() && k < st.texel_ranges.size(); ++k) {
        const auto& t = st.textures[k];
        if (!t || !st.texel_ranges[k].second || world_textures_.count(t->record)) continue;
        std::string why;
        Renderer::Image* made = st.images && k < st.images->size() ? &(*st.images)[k] : nullptr;
        if (!begin_world_texture(*t, st.texels->buffer, st.texel_ranges[k].first, st.texel_ranges[k].second, p->cmd,
                                 made, why))
          notes_.push_back("world texture " + t->texture.name + ": " + why);  // drawn flat
      }
    }
    vkEndCommandBuffer(p->cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &p->cmd;
    if (vkQueueSubmit(r_.queue_, 1, &submit, p->fence) != VK_SUCCESS) {
      vkDestroyFence(r_.device_, p->fence, nullptr);
      p->fence = VK_NULL_HANDLE;
      pending_world_ = std::move(p);
      drop_pending_world();
      error = "world upload submit";
      return false;
    }
  }
  pending_world_ = std::move(p);
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
  } else {
    world_draws_.clear();
    world_triangles_ = 0;
  }
  world_origin_[0] = p.staged.origin_x;
  world_origin_[1] = p.staged.origin_z;
  drop_pending_world();
  if (swapped) *swapped = true;
  return true;
}

bool Showroom::set_world(StagedWorld staged, std::string& error) {
  return begin_world(std::move(staged), error) && poll_world(true, nullptr);
}

void Showroom::drop_fading_world(bool now) {
  if (!fading_on_) return;
  fading_on_ = false;
  fading_.draws.clear();
  if (now) {
    r_.destroy_buffer(fading_.vertices);
    r_.destroy_buffer(fading_.indices);
  } else {
    retire([this, v = fading_.vertices, i = fading_.indices]() mutable {
      r_.destroy_buffer(v);
      r_.destroy_buffer(i);
    });
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
  if (world_pool_left_ == 0) {
    constexpr uint32_t kSets = 256;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 3};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
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
  alloc.pSetLayouts = &material_layout_;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &out), "world material set");
  --world_pool_left_;
  return true;
}

bool Showroom::upload_world_textures(const std::vector<std::shared_ptr<const WorldTextureData>>& textures,
                                     std::string& error) {
  auto write_set = [&](VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo images[3];
    VkWriteDescriptorSet writes[3];
    for (uint32_t b = 0; b < 3; ++b) {
      images[b] = {sampler_, b == 0 ? view : gpu_textures_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[b].dstSet = set;
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(r_.device_, 3, writes, 0, nullptr);
  };
  if (!world_white_set_) {
    if (gpu_textures_.empty()) {
      error = "no built-in white texture";
      return false;
    }
    if (!allocate_world_set(world_white_set_, error)) return false;
    write_set(world_white_set_, gpu_textures_[0].view);
  }
  for (const auto& t : textures) {
    if (!t || world_textures_.count(t->record)) continue;
    GpuTexture image;
    std::string why;
    if (!upload_texture(&t->texture, t->srgb, image, why)) {
      r_.destroy_image(image);
      notes_.push_back("world texture " + t->texture.name + ": " + why);
      continue;  // drawn flat
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!allocate_world_set(set, error)) {
      r_.destroy_image(image);
      return false;
    }
    write_set(set, image.view);
    {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      world_textures_[t->record] = image;
    }
    world_sets_[t->record] = set;
  }
  return true;
}

VkDescriptorSet Showroom::world_layer_set(const std::array<uint32_t, 3>& records) {
  const auto it = world_layer_sets_.find(records);
  if (it != world_layer_sets_.end()) return it->second;
  VkDescriptorImageInfo images[3];
  VkWriteDescriptorSet writes[3];
  for (uint32_t b = 0; b < 3; ++b) {
    VkImageView view = gpu_textures_.empty() ? VK_NULL_HANDLE : gpu_textures_[0].view;
    if (records[b] != kNoWorldTexture) {
      const auto t = world_textures_.find(records[b]);
      if (t == world_textures_.end()) return VK_NULL_HANDLE;
      view = t->second.view;
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
  vkUpdateDescriptorSets(r_.device_, 3, writes, 0, nullptr);
  world_layer_sets_[records] = set;
  return set;
}

VkDescriptorSet Showroom::world_set(uint32_t record) const {
  const auto it = world_sets_.find(record);
  return it == world_sets_.end() ? VK_NULL_HANDLE : it->second;
}

void Showroom::record_world(VkCommandBuffer cmd) {
  if (world_draws_.empty() || material_sets_.empty()) return;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &world_vertices_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, world_indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  if (world_textured_) {
    world_stats_ = {};
    float swap = 2.0f;
    if (fading_on_) {
      const double t =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - fade_start_).count() / swap_fade_seconds_;
      if (t >= 1.0) drop_fading_world(false);
      else swap = float(std::max(t, 0.0));
    }
    record_world_textured(cmd, world_draws_, world_transform_, swap, false);
    if (fading_on_) {
      // The outgoing scenery's positions are relative to its own origin:
      // shifted by the origins' difference into the transform.
      const float dx = float(fading_.origin[0] - world_origin_[0]), dz = float(fading_.origin[1] - world_origin_[1]);
      float m[16];
      std::memcpy(m, world_transform_, sizeof m);
      for (int r = 0; r < 4; ++r) m[12 + r] += world_transform_[r] * dx + world_transform_[8 + r] * dz;
      VkDeviceSize zero = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &fading_.vertices.buffer, &zero);
      vkCmdBindIndexBuffer(cmd, fading_.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
      record_world_textured(cmd, fading_.draws, m, 3.0f + swap, true);
    }
    return;
  }
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &material_sets_[0], 0,
                          nullptr);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_);
  for (const auto& d : world_draws_) {
    ShowroomPush push = d.push;
    std::memcpy(push.model, world_transform_, sizeof push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
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
                                     const float transform[16], float swap, bool outgoing) {
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
      if (outgoing && !d.cull) continue;  // the sky: the incoming one's is drawn whole
      if (d.cull && ((world_cull_ && outside(d)) || near_lod_beyond(d, eye))) {
        ++world_stats_.culled;
        continue;
      }
      ++world_stats_.drawn;
      world_stats_.indices += d.index_count;
      const bool sky = uint32_t(d.push.params[1] + 0.5f) == 2;
      if (group < 0 && !sky) {
        r_.mark(cmd, "sky");
        group = 0;
      }
      if (group >= 0 && group < 99 && d.blend != group) {
        r_.mark(cmd, kGroup[std::min(group, 2)]);
        group = d.blend;
      }
      // Additive (3) shares the blended pipeline: its fragments carry alpha 0.
      const VkPipeline pipeline = d.blend >= 2 ? world_alpha_ : d.blend == 1 ? world_coverage_ : world_opaque_;
      if (pipeline != bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
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
      ShowroomPush push = d.push;
      if (set && d.texture_b != kNoWorldTexture) {
        VkDescriptorSet layered = world_layer_set({d.texture, d.texture_b, d.splat});
        if (layered) set = layered;
        else push.params[1] = 1, ++world_stats_.layer_fallback;  // a layer did not upload: slot 0 alone
      }
      if (!set) {
        if (d.texture != kNoWorldTexture) ++world_stats_.white;
        set = world_white_set_;
        if (push.params[1] == 1) push.params[1] = 0;  // the texture did not upload: flat
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &(bound_set = set), 0,
                                nullptr);
      // Cut-outs without MSAA: world.frag's alpha test (flag 64).
      if (d.blend == 1 && r_.samples_ == VK_SAMPLE_COUNT_1_BIT)
        push.params[1] = float(uint32_t(push.params[1] + 0.5f) | 64u);
      std::memcpy(push.model, transform, sizeof push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = swap;  // world.frag's swap dither
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
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
  if (ok) {
    void* mapped = nullptr;
    vkMapMemory(r_.device_, readback.memory, 0, bytes, 0, &mapped);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    mask.resize(size_t(bytes));
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = fraction ? src[i] : (src[i] != 0 ? 255 : 0);
    vkUnmapMemory(r_.device_, readback.memory);
  } else {
    error = "mask render submission failed";
  }
  r_.destroy_buffer(readback);
  r_.destroy_image(target);
  r_.destroy_image(multi);
  return ok;
}

}  // namespace carview
