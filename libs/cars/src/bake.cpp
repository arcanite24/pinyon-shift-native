#include "cars/bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

#include "cars/util.h"
#include "cars/visual_setup.h"
#include "fh1data/carbin.h"
#include "fh1data/xds.h"

namespace cars {
namespace {

using bake::MaterialClass;
using bake::Role;

bool ends_with(const std::string& s, const std::string& t) {
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
bool starts_with(const std::string& s, const std::string& t) { return s.rfind(t, 0) == 0; }

bake::Texture texture_from_blocks(const std::string& name, fh1::XdsBlocks&& blocks) {
  bake::Texture t;
  t.name = name;
  t.format = bake::TextureFormat(uint32_t(blocks.format));
  t.swizzle = blocks.swizzle;
  t.levels.reserve(blocks.levels.size());
  for (auto& l : blocks.levels) t.levels.push_back({l.width, l.height, std::move(l.blocks)});
  return t;
}

// Every .xds of a folder (sorted by name), as block textures.
void bake_textures(Folder& folder, bake::Bake& out, std::vector<std::string>* notes) {
  size_t xds_count = 0;
  for (const std::string& name : folder.names())
    if (ends_with(name, ".xds")) ++xds_count;
  out.textures.reserve(out.textures.size() + xds_count);
  std::vector<uint8_t> data;
  std::string error;
  for (const std::string& name : folder.names()) {
    if (!ends_with(name, ".xds")) continue;
    data.clear();
    error.clear();
    fh1::XdsBlocks blocks;
    if (!folder.read(name, data, &error) || !fh1::extract_xds_blocks(data, blocks, &error)) {
      if (notes) notes->push_back("texture " + name + ": " + error);
      continue;
    }
    out.textures.push_back(texture_from_blocks(name, std::move(blocks)));
  }
}

bake::Vertex bake_vertex(const fh1::CarbinPart& part, const fh1::CarbinVertex& v) {
  bake::Vertex o;
  o.position = fh1::carbin_position(part, v);
  o.frame = v.frame;
  o.uv0 = v.uv0;
  o.uv1 = v.uv1;
  o.lighting = v.lighting;
  return o;
}

float half_to_float(uint16_t h) {
  const int sign = h >> 15, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v;
  if (exponent == 0)
    v = std::ldexp(float(mantissa), -24);
  else if (exponent == 31)
    v = mantissa ? NAN : INFINITY;
  else
    v = std::ldexp(float(mantissa | 0x400), exponent - 25);
  return sign ? -v : v;
}

uint32_t load_be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

int32_t sign_extend_bits(uint32_t value, int bits) {
  const uint32_t sign_bit = 1u << (bits - 1);
  return int32_t((value ^ sign_bit) - sign_bit);
}

void apply_crush_weights(std::vector<bake::Vertex>& verts, const std::vector<float>& weights) {
  for (size_t i = 0; i < verts.size(); ++i) {
    const float w = i < weights.size() ? std::clamp(weights[i], 0.0f, 1.0f) : 0.0f;
    verts[i].lighting[kCrushWeightLightingChannel] =
        uint8_t(std::clamp(std::lround(w * 255.0f), 0L, 255L));
  }
}

// A wheel or tyre pool's morph targets from its damage-tail block
// (FORMAT-CARS, Damage tail): after the buffer id, per vertex one or two
// targets of four half floats (a direction and a length, low half of each
// big-endian word first): position = xyz x w in metres, about the part's
// offset (not scaled by the half-extents as vertex positions are).
std::vector<bake::MorphTarget> morph_targets(const fh1::CarbinPart& part, const fh1::CarbinDamageBlock& d,
                                             size_t vertices) {
  std::vector<bake::MorphTarget> out;
  if (d.count != vertices || vertices == 0 || (d.stride != 8 && d.stride != 16)) return out;
  std::vector<uint8_t> bytes = d.table;
  bytes.insert(bytes.end(), d.words.begin(), d.words.end());
  const size_t body = size_t(d.count) * d.stride;
  if (bytes.size() < body) return out;
  const size_t skip = bytes.size() - body;  // the buffer id
  const size_t targets = d.stride / 8;
  out.assign(targets, bake::MorphTarget(vertices));
  for (size_t v = 0; v < vertices; ++v)
    for (size_t t = 0; t < targets; ++t) {
      const uint8_t* at = bytes.data() + skip + v * d.stride + t * 8;
      const uint32_t w0 = load_be32(at), w1 = load_be32(at + 4);
      const float h[4] = {half_to_float(uint16_t(w0 & 0xFFFF)), half_to_float(uint16_t(w0 >> 16)),
                          half_to_float(uint16_t(w1 & 0xFFFF)), half_to_float(uint16_t(w1 >> 16))};
      for (int a = 0; a < 3; ++a) out[t][v][size_t(a)] = h[a] * h[3] + part.bounds[size_t(a)];
    }
  return out;
}

Role role_of(const std::string& name, bake::Corner& corner) {
  corner = bake::Corner::kNone;
  for (auto [tag, c] : {std::pair{"lf_", bake::Corner::kLF}, std::pair{"rf_", bake::Corner::kRF},
                        std::pair{"lr_", bake::Corner::kLR}, std::pair{"rr_", bake::Corner::kRR}}) {
    for (const char* kind : {"caliper", "rotor"}) {
      if (name.find(std::string(kind) + tag) != std::string::npos) corner = c;
    }
  }
  if (name.find("caliper") != std::string::npos) return Role::kCaliper;
  if (name.find("rotor") != std::string::npos) return Role::kRotor;
  if (ends_with(name, "_cockpit.carbin")) return Role::kCockpit;
  if (ends_with(name, "_lod0.carbin")) return Role::kLod0;
  return Role::kExterior;
}

// Texture slots of a sub-section (FORMAT-CARS "Texture slots"); names must
// exist in their pack or the slot is left empty and noted.
void assign_slots(bake::Subsection& s, Role role, const std::set<std::string>& own,
                  const std::set<std::string>& shared, std::vector<std::string>& notes) {
  const std::string t = lower(s.technique);
  const bool lod0 = s.lod == 0;
  auto own_ref = [&](const std::string& lod0_name, const std::string& lod_name) -> std::string {
    const std::string& n = lod0 ? lod0_name : lod_name;
    if (own.count(n)) return (role == Role::kWheel || role == Role::kTyre ? "wheel:" : "car:") + n;
    if (own.count(lod0_name)) return (role == Role::kWheel || role == Role::kTyre ? "wheel:" : "car:") + lod0_name;
    notes.push_back("no " + n + " for technique " + s.technique);
    return "";
  };
  auto shared_ref = [&](const std::string& n) -> std::string {
    if (shared.empty() || shared.count(n)) return "shared:" + n;
    notes.push_back("no shared " + n + " for technique " + s.technique);
    return "";
  };
  if (role == Role::kWheel) {
    s.slots[0] = own_ref("wheel_lod0.xds", "wheel.xds");
    if (is_blur_rim_technique(t) && own.count("wheel0.xds")) {
      s.slots[1] = "wheel:wheel0.xds";
    }
    return;
  }
  if (role == Role::kTyre) {
    // The tyre textures are taken from the car's tyre texture archive.
    if (t == "sidewall") s.slots[1] = "tyre:sidewall";
    if (t == "tread") s.slots[1] = "tyre:tread";
    return;
  }
  s.slots[0] = "@atlas";
  switch (s.material) {
    case MaterialClass::kInterior:
    case MaterialClass::kGauge:
      s.slots[1] = own_ref("interior_lod0.xds", "interior_lod0.xds");
      break;
    case MaterialClass::kLamp:
      s.slots[1] = own_ref("lights_lod0.xds", "lights.xds");
      break;
    case MaterialClass::kCarbon:
      s.slots[1] = shared_ref("carbonfiber.xds");
      break;
    case MaterialClass::kGlass:
      if (starts_with(t, "detail_glass_")) s.slots[1] = own_ref("lights_lod0.xds", "lights.xds");
      break;
    default:
      if (t == "numplate") s.slots[1] = own_ref("lights_lod0.xds", "lights.xds");
      if (t == "bumper_frame") s.slots[1] = shared_ref("bumper_frame.xds");
      // The grilles: FORMAT-CARS gives grille1 the Shared grille1_alpha in
      // slot 1 (frame dump). Ours: grille1_alpha, grille2 and grille2_alpha
      // take their family's alpha texture the same way, since every grille
      // section's UV0 tiles (4 to 77 times), which fits the 64 x 64 Shared
      // tile and not the atlas.
      if (t == "grille1" || t == "grille1_alpha") s.slots[1] = shared_ref("grille1_alpha.xds");
      if (t == "grille2" || t == "grille2_alpha") s.slots[1] = shared_ref("grille2_alpha.xds");
      break;
  }
}

bool parse_model(Folder& folder, const std::string& name, Role role, bake::Corner corner,
                 const std::set<std::string>& own, const std::set<std::string>& shared, bake::Model& out,
                 std::vector<std::string>& notes, std::string* error) {
  std::vector<uint8_t> data;
  if (!folder.read(name, data, error)) return false;
  fh1::Carbin carbin;
  if (!fh1::parse_carbin(data, carbin, error)) {
    if (error) *error = name + ": " + *error;
    return false;
  }
  const auto transparent = fh1::carbin_transparent_flags(carbin);
  out.name = name;
  out.role = role;
  out.corner = corner;
  out.header_kind = carbin.kind;
  if (carbin.kind == 3 && carbin.head.size() >= 48)
    for (size_t k = 0; k < 4; ++k) {
      const uint32_t bits = load_be32(carbin.head.data() + 32 + 4 * k);
      std::memcpy(&out.header_bounds[k], &bits, 4);
    }
  out.parts.reserve(carbin.parts.size());
  for (size_t p = 0; p < carbin.parts.size(); ++p) {
    const fh1::CarbinPart& part = carbin.parts[p];
    if (part.empty_box()) continue;  // no geometry
    bake::Part bp;
    bp.name = part.name;
    bp.bounds = part.bounds;
    bp.lod_vertices.reserve(part.lod_pool.vertices.size());
    for (const auto& v : part.lod_pool.vertices) bp.lod_vertices.push_back(bake_vertex(part, v));
    bp.lod0_vertices.reserve(part.lod0_pool.vertices.size());
    for (const auto& v : part.lod0_pool.vertices) bp.lod0_vertices.push_back(bake_vertex(part, v));
    if ((role == Role::kWheel || role == Role::kTyre) && part.damage.size() == 2) {
      bp.lod_morphs = morph_targets(part, part.damage[0], bp.lod_vertices.size());
      bp.lod0_morphs = morph_targets(part, part.damage[1], bp.lod0_vertices.size());
      apply_crush_weights(bp.lod_vertices, {});
      apply_crush_weights(bp.lod0_vertices, {});
    } else if (role != Role::kWheel && role != Role::kTyre && part.damage_flag == 1) {
      const std::vector<float> lod_w =
          part.damage.size() >= 1 ? decode_carbin_crush_weights(part, part.damage[0], bp.lod_vertices.size())
                                  : std::vector<float>{};
      const std::vector<float> lod0_w =
          part.damage.size() >= 2 ? decode_carbin_crush_weights(part, part.damage[1], bp.lod0_vertices.size())
                                  : std::vector<float>{};
      apply_crush_weights(bp.lod_vertices, lod_w);
      apply_crush_weights(bp.lod0_vertices, lod0_w);
    } else {
      apply_crush_weights(bp.lod_vertices, {});
      apply_crush_weights(bp.lod0_vertices, {});
    }
    bp.subsections.reserve(part.subsections.size());
    for (size_t i = 0; i < part.subsections.size(); ++i) {
      const fh1::CarbinSubsection& s = part.subsections[i];
      bake::Subsection bs;
      bs.technique = s.material;
      bs.transparent = transparent[p][i];
      bs.lod = s.lod;
      bs.uv_transform = s.uv_transform;
      bs.material = classify(s.material, bs.transparent, role);
      bs.indices = fh1::carbin_triangle_list(s);
      assign_slots(bs, role, own, shared, notes);
      bp.subsections.push_back(std::move(bs));
    }
    out.parts.push_back(std::move(bp));
  }
  return true;
}

// <Name attr="value" ... /> elements of Masks.xml.
std::vector<bake::LiveryPlane> parse_masks(const std::string& xml) {
  std::vector<bake::LiveryPlane> planes;
  size_t at = 0;
  while ((at = xml.find('<', at)) != std::string::npos) {
    ++at;
    if (at >= xml.size() || xml[at] == '?' || xml[at] == '/' || xml[at] == '!') continue;
    size_t end = xml.find('>', at);
    if (end == std::string::npos) break;
    const std::string body = xml.substr(at, end - at);
    at = end;
    size_t name_end = body.find_first_of(" \t\r\n/");
    const std::string name = body.substr(0, name_end);
    if (name == "LiveryMasks" || name.empty()) continue;
    bake::LiveryPlane p;
    p.name = name;
    auto attr = [&](const std::string& key) -> std::string {
      const std::string k = key + "=\"";
      size_t a = 0;
      while ((a = body.find(k, a)) != std::string::npos) {
        if (a == 0 || std::isspace(static_cast<unsigned char>(body[a - 1]))) break;
        ++a;
      }
      if (a == std::string::npos) return "";
      const size_t v = a + k.size();
      return body.substr(v, body.find('"', v) - v);
    };
    auto num = [&](const std::string& key, float fallback) {
      const std::string v = attr(key);
      try {
        return v.empty() ? fallback : std::stof(v);
      } catch (...) {
        return fallback;
      }
    };
    p.valid = lower(attr("valid")) == "true";
    p.left = num("left", 0);
    p.right = num("right", 0);
    p.top = num("top", 0);
    p.bottom = num("bottom", 0);
    p.x_origin = num("xorigin", 0);
    p.y_origin = num("yorigin", 0);
    p.x_scale = num("xScale", 1);
    p.y_scale = num("yScale", 1);
    p.rotation = num("rotation", 0);
    p.x_axis = attr("xAxis");
    p.y_axis = attr("yAxis");
    planes.push_back(p);
  }
  return planes;
}

void bake_livery(Folder& folder, bake::Bake& out, std::vector<std::string>& notes) {
  std::vector<uint8_t> data;
  std::string error;
  if (!folder.contains("liverymasks/masks.xml")) return;
  if (!folder.read("liverymasks/masks.xml", data, &error)) {
    notes.push_back("livery: " + error);
    return;
  }
  out.livery = parse_masks(std::string(data.begin(), data.end()));
  // One R8 sheet: the maximum alpha of every valid plane's mask.
  bake::Texture sheet;
  sheet.name = "livery_mask";
  sheet.format = bake::TextureFormat::kR8;
  sheet.swizzle = {0, 0, 0, 5};
  bake::TextureLevel level;
  for (const auto& p : out.livery) {
    if (!p.valid) continue;
    const std::string file = "liverymasks/" + lower(p.name) + ".tga";
    if (!folder.contains(file)) {
      notes.push_back("livery plane " + p.name + " has no " + file);
      continue;
    }
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!folder.read(file, data, &error) || !decode_tga(data, w, h, rgba, &error)) {
      notes.push_back("livery " + file + ": " + error);
      continue;
    }
    if (level.data.empty()) {
      level.width = w;
      level.height = h;
      level.data.assign(size_t(w) * h, 0);
    }
    if (w != level.width || h != level.height) {
      notes.push_back("livery " + file + " is " + std::to_string(w) + "x" + std::to_string(h) +
                      ", not the sheet's size: left out");
      continue;
    }
    for (size_t i = 0; i < level.data.size(); ++i) level.data[i] = std::max(level.data[i], rgba[4 * i + 3]);
  }
  if (!level.data.empty()) {
    sheet.levels.push_back(std::move(level));
    out.textures.push_back(std::move(sheet));
  }
}

}  // namespace

BlurRimKind classify_blur_rim(std::string_view technique) {
  const std::string t = lower(std::string(technique));
  if (t == "blur_rim") return BlurRimKind::kBlurRim;
  if (t == "chrome_blur_rim") return BlurRimKind::kChromeBlurRim;
  if (t == "blur_lip") return BlurRimKind::kBlurLip;
  if (t == "chrome_blur_lip") return BlurRimKind::kChromeBlurLip;
  return BlurRimKind::kNone;
}

bool is_blur_rim_technique(std::string_view technique) {
  return classify_blur_rim(technique) != BlurRimKind::kNone;
}

float vertex_crush_weight(const bake::Vertex& v) {
  return float(v.lighting[kCrushWeightLightingChannel]) / 255.0f;
}

std::vector<float> decode_carbin_crush_weights(const fh1::CarbinPart& part,
                                               const fh1::CarbinDamageBlock& block,
                                               size_t vertex_count) {
  std::vector<float> out;
  if (block.count != vertex_count || vertex_count == 0 || block.stride != 4) return out;
  std::vector<uint8_t> bytes = block.table;
  bytes.insert(bytes.end(), block.words.begin(), block.words.end());
  const size_t body = size_t(block.count) * 4u;
  if (bytes.size() < body) return out;
  const size_t skip = bytes.size() - body;

  const float hx = part.bounds[6];
  const float hy = part.bounds[7];
  const float hz = part.bounds[8];
  float half_extent = std::sqrt(hx * hx + hy * hy + hz * hz);
  if (!std::isfinite(half_extent) || half_extent < 1e-4f) half_extent = 1.0f;

  out.resize(vertex_count, 0.0f);
  for (size_t i = 0; i < vertex_count; ++i) {
    const uint32_t w = load_be32(bytes.data() + skip + i * 4u);
    // HEND3N packed signed 11-11-10 crush displacement vector in part-local units.
    const float dx = float(sign_extend_bits(w & 0x7FFu, 11)) / 1023.0f;
    const float dy = float(sign_extend_bits((w >> 11) & 0x7FFu, 11)) / 1023.0f;
    const float dz = float(sign_extend_bits((w >> 22) & 0x3FFu, 10)) / 511.0f;
    const float disp_m = std::sqrt(dx * dx + dy * dy + dz * dz) * half_extent;
    out[i] = std::clamp(disp_m / kCrushWeightRefMeters, 0.0f, 1.0f);
  }
  return out;
}

MaterialClass classify(const std::string& technique, bool transparent, Role role) {
  (void)role;
  const std::string t = lower(technique);
  // Spinning-rim blur cards (cross-faded with wheel0..3.xds at speed): never kHidden or kGlass.
  if (t == "blur_rim" || t == "blur_lip") return MaterialClass::kRim;
  if (t == "chrome_blur_rim" || t == "chrome_blur_lip") return MaterialClass::kChrome;
  // Rear-window heater lines (69 base cars): translucent glass overlay, never opaque @atlas.
  if (t == "defrost_lines") return MaterialClass::kGlass;
  if (transparent) return MaterialClass::kGlass;
  if (t == "black_glass_breakable") return MaterialClass::kGlass;
  // Classify wheel/brake techniques regardless of model role so embedded rims/rotors
  // inside _lod0.carbin or kRotor never fall through to kOpaque.
  if (t == "rim" || t == "inner_rim" || t == "outer_rim" || t == "wheel_black" ||
      t == "hub" || t == "rotor" || t == "brake" || t == "drum")
    return MaterialClass::kRim;
  if (t == "tire" || t == "sidewall" || t == "tread") return MaterialClass::kTyre;
  if (t == "body" || t == "body_2" || t == "body_3" || t == "body_simple" || t == "headlight_paint")
    return MaterialClass::kPaint;
  if (t == "caliper") return MaterialClass::kCaliper;
  static const std::set<std::string> kLampExact = {
      "head_light", "head_light2", "headlightl_lod0", "headlightr_lod0",
      "hidhead", "oldhead", "xenonhead", "fullbeam",
      "taillight2s", "taillightst", "taillightl_lod0", "taillightr_lod0",
      "drlwhite", "drlwhiteswitch", "drlred", "drlorange",
      "drlbrakered", "drlbrakeorange", "slorange", "slinlorange", "slinrorange",
      "lindtailside", "rindtailside", "linred", "rinred", "brinl", "brinr",
      "brkreverse", "white_brake", "fogred", "fogwhite",
      "textured_reflector", "reflector"};
  if (kLampExact.count(t) || starts_with(t, "tail_light") || starts_with(t, "indicator_") ||
      starts_with(t, "reverse_light") || starts_with(t, "lights_gls_") ||
      starts_with(t, "drl") || starts_with(t, "slin"))
    return MaterialClass::kLamp;
  static const std::set<std::string> kInterior = {
      "leather", "cloth", "plastic", "solid", "mottled", "headliner",
      "bump_leather", "bump_plastic", "bump_stitching",
      "metal", "rubber", "stitching", "wood", "interior", "suede"};
  std::string base = t;
  while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
  if (kInterior.count(t) || (!base.empty() && kInterior.count(base))) return MaterialClass::kInterior;
  if (t == "gauge_emissive" || t == "z_gauge_emissive_alpha" || t == "gauge_emissive_alpha" ||
      t == "gauge_needle" || t == "cockpit_gauge_needle" || t == "digital_gauge" ||
      t == "cockpit_digital_gauge" || starts_with(t, "cockpit_gauge_emissive"))
    return MaterialClass::kGauge;
  if (starts_with(t, "chrome")) return MaterialClass::kChrome;
  if (t == "carbon_fiber" || t == "unicolor_carbon_fiber" || t == "carbon_fiber2" || t == "carbon_fiber_no_env")
    return MaterialClass::kCarbon;
  return MaterialClass::kOpaque;
}

bool decode_tga(const std::vector<uint8_t>& d, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba,
                std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (d.size() < 18) return fail("TGA too small");
  const uint8_t id_length = d[0], colour_map = d[1], type = d[2];
  width = uint32_t(d[12] | d[13] << 8);
  height = uint32_t(d[14] | d[15] << 8);
  const uint32_t bpp = d[16], descriptor = d[17];
  if (colour_map || (type != 2 && type != 10) || (bpp != 24 && bpp != 32))
    return fail("unsupported TGA (only true colour, 24 or 32 bits)");
  const size_t px = bpp / 8, count = size_t(width) * height;
  size_t at = 18 + id_length;
  std::vector<uint8_t> raw(count * 4);
  auto put = [&](size_t i, const uint8_t* p) {
    raw[4 * i] = p[2];
    raw[4 * i + 1] = p[1];
    raw[4 * i + 2] = p[0];
    raw[4 * i + 3] = px == 4 ? p[3] : 255;
  };
  if (type == 2) {
    if (at + count * px > d.size()) return fail("TGA truncated");
    for (size_t i = 0; i < count; ++i) put(i, &d[at + i * px]);
  } else {
    size_t i = 0;
    while (i < count) {
      if (at >= d.size()) return fail("TGA truncated");
      const uint8_t head = d[at++];
      const size_t n = (head & 0x7F) + 1u;
      if (i + n > count) return fail("TGA run overflows");
      if (head & 0x80) {
        if (at + px > d.size()) return fail("TGA truncated");
        for (size_t k = 0; k < n; ++k) put(i + k, &d[at]);
        at += px;
      } else {
        if (at + n * px > d.size()) return fail("TGA truncated");
        for (size_t k = 0; k < n; ++k) put(i + k, &d[at + k * px]);
        at += n * px;
      }
      i += n;
    }
  }
  // Rows are stored bottom first unless descriptor bit 5 is set.
  const bool top_first = (descriptor & 0x20) != 0;
  if (top_first) {
    rgba = std::move(raw);
  } else {
    rgba.resize(raw.size());
    const size_t row_bytes = size_t(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
      const uint32_t src = height - 1 - y;
      std::memcpy(&rgba[size_t(y) * row_bytes], &raw[size_t(src) * row_bytes], row_bytes);
    }
  }
  return true;
}

bool bake_car(const FolderRef& ref, const CarRow& row, bake::Bake& out, std::string* error) {
  out = bake::Bake{};
  out.kind = bake::Kind::kCar;
  Folder folder;
  if (!folder.open(ref, error)) return false;
  bake::CarInfo& c = out.car;
  std::vector<std::string>& notes = c.notes;
  c.media_name = row.media_name;
  c.display_name = row.display_name;
  c.origin = ref.origin;
  c.car_id = row.id;
  c.wheel_media = lower(row.wheel_media);
  const std::string own_tyre = per_car_tyre_media(row.media_name);
  c.tyre_media = own_tyre.empty() ? kTyreModel : own_tyre;
  c.tyre_texture_media = own_tyre.empty() ? kTyreTextures : own_tyre;
  c.tyre_width_mm = row.tyre_width_mm;
  c.tyre_aspect = row.tyre_aspect;
  c.rim_inches = row.rim_inches;
  c.ride_height = {float(row.ride_height[0]), float(row.ride_height[1])};
  c.caliper_rgb = {row.caliper_rgb_front, row.caliper_rgb_rear};
  c.caliper_angle = {float(row.caliper_angle_front), float(row.caliper_angle_rear)};
  c.has_livery = row.has_livery;
  c.wing_mask = row.wing_mask;
  for (size_t i = 0; i < 10; ++i)
    for (size_t a = 0; a < 3; ++a) c.part_positions[i][a] = float(row.part_positions[i][a]);
  c.has_body = row.has_body;
  c.track_outer = {float(row.track_outer[0] + 2.0 * row.track_offset[0]),
                   float(row.track_outer[1] + 2.0 * row.track_offset[1])};
  c.wheelbase = float(row.wheelbase);
  c.body_bottom_y = float(row.body_bottom_y);
  c.has_track_offsets = row.has_track_offsets;
  if (!row.has_body) notes.push_back("no Data_CarBody row " + std::to_string(row.id * 1000));
  // The model-to-body pose (FORMAT-CARS, Wheels): z mirrored, lifted by the
  // mean stock ride height less the body's bottom, pitched by the ride
  // height difference over the wheelbase.
  const double lift = (row.ride_height[0] + row.ride_height[1]) / 2 - row.body_bottom_y;
  const double pitch = row.wheelbase > 0 ? std::atan((row.ride_height[1] - row.ride_height[0]) / row.wheelbase) : 0;
  c.lift = float(lift);
  c.pitch = float(pitch);
  // LF, RF, LR, RR from CarPartPositions PartId 5 LF, 6 RF, 8 LR, 7 RR
  // (body frame, +Z forward). Only z is taken from the rows; x is half the
  // outer track less the tyre width (plus CarTrackOffsets), y the tyre's outer radius.
  const size_t wheel_rows[4] = {5, 6, 8, 7};
  for (size_t w = 0; w < 4; ++w) {
    const size_t axle = w < 2 ? 0 : 1;
    const double rim_radius = double(row.rim_inches[axle]) * 0.0254 / 2;
    const double width = double(row.tyre_width_mm[axle]) / 1000;
    const double outer = rim_radius + width * double(row.tyre_aspect[axle]) / 100;
    const double side = (w % 2 == 0) ? -1 : 1;  // left at -x
    const double track_x = (row.track_outer[axle] - width) / 2 + row.track_offset[axle];
    const double body[3] = {side * track_x, outer, row.part_positions[wheel_rows[w]][2]};
    const double cs = std::cos(pitch), sn = std::sin(pitch);
    const double y = body[1] - lift, z = body[2];
    c.wheel_centres_body[w] = {float(body[0]), float(body[1]), float(body[2])};
    c.wheel_centres[w] = {float(body[0]), float(cs * y + sn * z), float(sn * y - cs * z)};
    if (!row.has_part_position[wheel_rows[w]]) notes.push_back("no CarPartPositions row " + std::to_string(wheel_rows[w]));
  }

  std::set<std::string> own;
  for (const auto& n : folder.names())
    if (ends_with(n, ".xds")) own.insert(n);
  for (const CarColour& col : row.colours) {
    bake::Colour bc;
    bc.id = col.id;
    bc.sequence = col.sequence;
    bc.metallic = col.metallic;
    bc.rgb = col.rgb;
    bc.has_stripe = col.has_stripe;
    bc.stripe_rgb = col.stripe_rgb;
    std::string stem = lower(col.base_texture);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos) stem = stem.substr(0, dot);
    if (stem.empty()) stem = "nodamage";
    bc.atlas_lod0 = own.count(stem + "_lod0.xds") ? stem + "_lod0.xds" : own.count(stem + ".xds") ? stem + ".xds" : "";
    bc.atlas_lod = own.count(stem + ".xds") ? stem + ".xds" : "";
    if (bc.atlas_lod0.empty()) {
      notes.push_back("colour " + std::to_string(col.id) + ": no atlas for " + col.base_texture + "; nodamage_lod0.xds");
      bc.atlas_lod0 = own.count("nodamage_lod0.xds") ? "nodamage_lod0.xds" : "";
    }
    if (bc.atlas_lod.empty()) bc.atlas_lod = own.count("nodamage.xds") ? "nodamage.xds" : bc.atlas_lod0;
    if (bc.atlas_lod0 == stem + ".xds" && stem != "nodamage")
      notes.push_back("colour " + std::to_string(col.id) + ": " + col.base_texture + " has no _lod0 atlas; " +
                      stem + ".xds for LOD0");
    c.colours.push_back(bc);
  }
  if (c.colours.empty()) notes.push_back("no Combo_Colors row");
  else if (c.colours[0].metallic) notes.push_back("first colour is metallic: engine finish");

  for (const std::string& name : folder.names()) {
    if (name.find('/') != std::string::npos || !ends_with(name, ".carbin") || starts_with(name, "stripped_")) continue;
    bake::Corner corner;
    const Role role = role_of(name, corner);
    bake::Model model;
    std::string why;
    if (!parse_model(folder, name, role, corner, own, {}, model, notes, &why)) {
      if (error) *error = ref.name + "/" + why;
      return false;
    }
    if (model.parts.empty()) notes.push_back(name + " has no parts");
    out.models.push_back(std::move(model));
  }
  bake_textures(folder, out, &notes);
  bake_livery(folder, out, notes);
  std::sort(notes.begin(), notes.end());
  notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
  return true;
}

bool bake_wheel(const FolderRef& ref, bool tyre, bake::Bake& out, std::string* error) {
  out = bake::Bake{};
  out.kind = bake::Kind::kWheel;
  Folder folder;
  if (!folder.open(ref, error)) return false;
  std::set<std::string> own;
  for (const auto& n : folder.names())
    if (ends_with(n, ".xds")) own.insert(n);
  std::vector<std::string> notes;
  for (const std::string& name : folder.names()) {
    if (name.find('/') != std::string::npos || !ends_with(name, ".carbin") || starts_with(name, "stripped_")) continue;
    bake::Model model;
    if (!parse_model(folder, name, tyre ? Role::kTyre : Role::kWheel, bake::Corner::kNone, own, {}, model, notes,
                     error))
      return false;
    out.models.push_back(std::move(model));
  }
  bake_textures(folder, out, &notes);
  return true;
}

bool bake_shared(const FolderRef& ref, bake::Bake& out, std::string* error) {
  out = bake::Bake{};
  out.kind = bake::Kind::kShared;
  Folder folder;
  if (!folder.open(ref, error)) return false;
  bake_textures(folder, out, nullptr);
  return true;
}

bool load_wheel_from_disc(const std::filesystem::path& game_root, const std::string& wheel_media, bool tyre,
                          bake::Bake& out, std::string* error) {
  namespace fs = std::filesystem;
  if (game_root.empty() || wheel_media.empty()) {
    if (error) *error = "load_wheel_from_disc: empty game_root or wheel_media";
    return false;
  }
  const std::string target = lower(wheel_media) + ".zip";
  const fs::path wheels_dir = game_root / "media" / "wheels";
  std::error_code ec;
  if (!fs::is_directory(wheels_dir, ec)) {
    if (error) *error = "wheels dir not found: " + wheels_dir.string();
    return false;
  }
  fs::path found;
  for (const auto& entry : fs::directory_iterator(wheels_dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (lower(entry.path().filename().string()) == target) {
      found = entry.path();
      break;
    }
  }
  if (found.empty()) {
    if (error) *error = "wheel archive not found: " + target;
    return false;
  }
  FolderRef ref;
  ref.kind = FolderRef::Kind::kArchive;
  ref.name = lower(wheel_media);
  ref.path = found;
  ref.origin = "disc";
  return bake_wheel(ref, tyre, out, error);
}

bool load_brake_model(const std::filesystem::path& game_root, int size_in, int type_id, int pistons, bool is_rotor,
                      bake::Model& out, std::string* error) {
  namespace fs = std::filesystem;
  out = bake::Model{};
  if (game_root.empty() || size_in <= 0) {
    if (error) *error = "load_brake_model: invalid game_root or size";
    return false;
  }
  const fs::path brakes_dir = game_root / "media" / "brakes";
  std::error_code ec;
  if (!fs::is_directory(brakes_dir, ec)) {
    if (error) *error = "brakes dir not found: " + brakes_dir.string();
    return false;
  }

  // Clamp size to available range (7..17) and pistons to {2,4,6,8}.
  const int sz = std::clamp(size_in, 7, 17);
  const bool drum_null = (!is_rotor && type_id == 1);
  std::vector<std::string> candidates;
  if (drum_null) {
    // Drum brakes (List_BrakeType ID 1) use the null caliper model rather than a floating disc caliper.
    candidates.push_back("drum_null_caliper.rmb.carbin");
  } else if (is_rotor) {
    const std::string stem = brake_type_stem(type_id);
    candidates.push_back(std::to_string(sz) + "_" + stem + "_lod0.rmb.carbin");
    candidates.push_back(std::to_string(sz) + "_" + stem + ".rmb.carbin");
    if (type_id == 1) {
      const int drum_sz = std::clamp(sz, 7, 10);
      candidates.push_back(std::to_string(drum_sz) + "_drum_lod0.rmb.carbin");
      candidates.push_back(std::to_string(drum_sz) + "_drum.rmb.carbin");
    }
    if (sz == 17) {
      candidates.push_back("16_" + stem + "_lod0.rmb.carbin");
      candidates.push_back("16_" + stem + ".rmb.carbin");
    }
    const int fallback_sz = std::min(sz, 16);
    candidates.push_back(std::to_string(fallback_sz) + "_rotor_steel_lod0.rmb.carbin");
    candidates.push_back(std::to_string(fallback_sz) + "_rotor_steel.rmb.carbin");
  } else {
    const int p = pistons >= 8 ? 8 : pistons >= 6 ? 6 : pistons >= 4 ? 4 : 2;
    candidates.push_back(std::to_string(sz) + "_caliper_" + std::to_string(p) + "_lod0.rmb.carbin");
    candidates.push_back(std::to_string(sz) + "_caliper_" + std::to_string(p) + ".rmb.carbin");
    if (sz == 17) {
      candidates.push_back("16_caliper_" + std::to_string(p) + "_lod0.rmb.carbin");
      candidates.push_back("16_caliper_" + std::to_string(p) + ".rmb.carbin");
    }
    const int fallback_sz = std::min(sz, 16);
    candidates.push_back(std::to_string(fallback_sz) + "_caliper_4_lod0.rmb.carbin");
    candidates.push_back(std::to_string(fallback_sz) + "_caliper_2.rmb.carbin");
  }

  // Case-insensitive lookup in media/brakes/.
  std::map<std::string, fs::path> files;
  for (const auto& entry : fs::directory_iterator(brakes_dir, ec)) {
    if (entry.is_regular_file(ec)) files[lower(entry.path().filename().string())] = entry.path();
  }
  fs::path chosen;
  std::string chosen_name;
  for (const auto& cand : candidates) {
    if (auto it = files.find(cand); it != files.end()) {
      chosen = it->second;
      chosen_name = cand;
      break;
    }
  }
  if (chosen.empty()) {
    if (error) *error = "brake model not found for size " + std::to_string(sz);
    return false;
  }

  std::ifstream in(chosen, std::ios::binary | std::ios::ate);
  if (!in) {
    if (error) *error = "cannot open " + chosen.string();
    return false;
  }
  const std::streamoff len = in.tellg();
  in.seekg(0);
  std::vector<uint8_t> data(len > 0 ? size_t(len) : 0u);
  if (!data.empty() && !in.read(reinterpret_cast<char*>(data.data()), len)) {
    if (error) *error = "cannot read " + chosen.string();
    return false;
  }

  fh1::Carbin carbin;
  if (!fh1::parse_carbin(data, carbin, error)) return false;
  const auto transparent = fh1::carbin_transparent_flags(carbin);
  const Role role = is_rotor ? Role::kRotor : Role::kCaliper;
  out.name = chosen_name;
  out.role = role;
  out.corner = bake::Corner::kNone;
  out.header_kind = carbin.kind;
  std::vector<std::string> notes;
  for (size_t p = 0; p < carbin.parts.size(); ++p) {
    const fh1::CarbinPart& part = carbin.parts[p];
    if (part.empty_box()) continue;
    bake::Part bp;
    bp.name = part.name;
    bp.bounds = part.bounds;
    for (const auto& v : part.lod_pool.vertices) bp.lod_vertices.push_back(bake_vertex(part, v));
    for (const auto& v : part.lod0_pool.vertices) bp.lod0_vertices.push_back(bake_vertex(part, v));
    apply_crush_weights(bp.lod_vertices, {});
    apply_crush_weights(bp.lod0_vertices, {});
    for (size_t i = 0; i < part.subsections.size(); ++i) {
      const fh1::CarbinSubsection& s = part.subsections[i];
      bake::Subsection bs;
      bs.technique = s.material;
      bs.transparent = transparent[p][i];
      bs.lod = s.lod;
      bs.uv_transform = s.uv_transform;
      bs.material = classify(s.material, bs.transparent, role);
      bs.indices = fh1::carbin_triangle_list(s);
      assign_slots(bs, role, {}, {}, notes);
      // Generic media/brakes/* meshes do not share UV layout with a car's exterior
      // paint @atlas: bind Shared/rotor0..4.xds for rotors/drums/hubs, and leave
      // generic caliper slot 0 empty so Data_Car/custom caliper paint tints cleanly.
      if (bs.slots[0] == "@atlas") {
        if (is_rotor && bs.material != MaterialClass::kCaliper) {
          const int rotor_idx = std::clamp(type_id - 1, 0, 4);
          bs.slots[0] = "shared:rotor" + std::to_string(rotor_idx) + ".xds";
        } else {
          bs.slots[0].clear();
        }
      }
      bp.subsections.push_back(std::move(bs));
    }
    out.parts.push_back(std::move(bp));
  }
  return drum_null || !out.parts.empty();
}

}  // namespace cars
