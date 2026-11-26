#include "world/world.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "fh1data/archive.h"
#include "fh1data/members.h"
#include "fh1data/placements.h"
#include "fh1data/props.h"
#include "fh1data/rmb.h"

namespace fs = std::filesystem;

namespace world {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// "name_LOD01_x" -> ("name_LOD**_x", 1); no LOD number -> (name, -1).
// Match the last "lodNN" (not preceded by "uber", e.g. "UberLOD23_LOD00" -> lod 0, not 23!).
std::string lod_family(const std::string& name, int& lod) {
  std::string n = lower(name);
  size_t best_at = std::string::npos;
  int best_lod = -1;
  for (size_t at = n.find("lod"); at != std::string::npos; at = n.find("lod", at + 1)) {
    if (at >= 4 && n.compare(at - 4, 4, "uber") == 0) continue;
    if (at + 4 < n.size() && std::isdigit(static_cast<unsigned char>(n[at + 3])) &&
        std::isdigit(static_cast<unsigned char>(n[at + 4]))) {
      best_lod = (n[at + 3] - '0') * 10 + (n[at + 4] - '0');
      best_at = at;
    }
  }
  if (best_at != std::string::npos) {
    lod = best_lod;
    n[best_at + 3] = n[best_at + 4] = '*';
    return n;
  }
  lod = -1;
  return n;
}

void object_bounds(const fh1::RmbObject& o, const fh1::RenderModel& model,
                   std::array<float, 3>& bmin, std::array<float, 3>& bmax) {
  const bool valid_he = std::isfinite(o.centre[0]) && std::isfinite(o.centre[1]) && std::isfinite(o.centre[2]) &&
                        std::isfinite(o.half_extent[0]) && std::isfinite(o.half_extent[1]) &&
                        std::isfinite(o.half_extent[2]) &&
                        (std::fabs(o.half_extent[0]) + std::fabs(o.half_extent[1]) + std::fabs(o.half_extent[2])) > 1e-4f;
  if (valid_he) {
    for (int i = 0; i < 3; ++i) {
      const float he = std::fabs(o.half_extent[size_t(i)]);
      bmin[size_t(i)] = o.centre[size_t(i)] - he;
      bmax[size_t(i)] = o.centre[size_t(i)] + he;
    }
  } else {
    bmin = model.box_min;
    bmax = model.box_max;
  }
}

float box_max_extent(const std::array<float, 3>& bmin, const std::array<float, 3>& bmax) {
  return std::max({0.0f, bmax[0] - bmin[0], bmax[1] - bmin[1], bmax[2] - bmin[2]});
}

// Bumped whenever what the index records changes (3: the far flag leaves
// out the CAGE, TERR_CUBE and occluder objects; 4: it also marks objects
// drawn only with the UberLOD effect), so an older index is rebuilt rather
// than reused.
constexpr const char* kCacheMagic = "shift-world-index 4";
// WORLD-MATERIALS, Placeholder models: the art tools' missing-material texture.
constexpr uint32_t kPlaceholderTextureId = 0x00002CA9u;

bool parse_finite_float(std::string_view sv, float& out) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
  if (sv.empty()) return false;
  std::string tmp(sv);
  char* end = nullptr;
  const float v = std::strtof(tmp.c_str(), &end);
  if (end == tmp.c_str() || !std::isfinite(v) || std::fabs(v) > 1e5f) return false;
  out = v;
  return true;
}

// Extracts a named attribute from an XML opening tag slice (case-insensitive).
bool xml_attr_float(std::string_view tag_body, std::string_view attr_name, float& out) {
  const std::string low_body = lower(std::string(tag_body));
  const std::string low_attr = lower(std::string(attr_name));
  size_t pos = 0;
  while ((pos = low_body.find(low_attr, pos)) != std::string::npos) {
    const bool start_ok = (pos == 0) || std::isspace(static_cast<unsigned char>(low_body[pos - 1]));
    size_t after = pos + low_attr.size();
    while (after < low_body.size() && std::isspace(static_cast<unsigned char>(low_body[after]))) ++after;
    if (start_ok && after < low_body.size() && low_body[after] == '=') {
      ++after;
      while (after < low_body.size() && std::isspace(static_cast<unsigned char>(low_body[after]))) ++after;
      if (after < tag_body.size() && (tag_body[after] == '"' || tag_body[after] == '\'')) {
        const char q = tag_body[after++];
        const size_t end_q = tag_body.find(q, after);
        if (end_q != std::string_view::npos) {
          return parse_finite_float(tag_body.substr(after, end_q - after), out);
        }
      }
    }
    pos += low_attr.size();
  }
  return false;
}

// Finds the opening tag body and optional inner content for the first matching XML element (case-insensitive).
bool find_xml_element(std::string_view xml, std::string_view elem_name,
                      std::string_view& out_tag_body, std::string_view& out_inner) {
  const std::string low_xml = lower(std::string(xml));
  const std::string needle = "<" + lower(std::string(elem_name));
  size_t pos = 0;
  while ((pos = low_xml.find(needle, pos)) != std::string::npos) {
    const size_t after = pos + needle.size();
    if (after < low_xml.size() &&
        (std::isspace(static_cast<unsigned char>(low_xml[after])) ||
         low_xml[after] == '>' || low_xml[after] == '/')) {
      const size_t gt = low_xml.find('>', after);
      if (gt == std::string::npos) return false;
      const bool self_closing = (gt > pos && low_xml[gt - 1] == '/');
      const size_t body_end = self_closing ? (gt - 1) : gt;
      out_tag_body = xml.substr(after, body_end - after);
      out_inner = {};
      if (!self_closing) {
        const std::string close_tag = "</" + lower(std::string(elem_name)) + ">";
        const size_t close_pos = low_xml.find(close_tag, gt + 1);
        if (close_pos != std::string::npos) {
          out_inner = xml.substr(gt + 1, close_pos - (gt + 1));
        }
      }
      return true;
    }
    pos = after;
  }
  return false;
}

}  // namespace

void WorldLodFilter::add(const std::string& family, int lod, const std::array<float, 3>& box_min,
                         const std::array<float, 3>& box_max) {
  if (lod < 0) return;
  by_family_[family].push_back({lod, box_min, box_max});
}

bool WorldLodFilter::suppressed(const std::string& family, int lod, const std::array<float, 3>& box_min,
                                const std::array<float, 3>& box_max, float max_gap) const {
  if (lod < 0) return false;
  const auto it = by_family_.find(family);
  if (it == by_family_.end()) return false;
  const float gap2 = max_gap * max_gap;
  for (const auto& e : it->second) {
    if (e.lod >= lod) continue;
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float g = std::max({0.0f, box_min[size_t(i)] - e.box_max[size_t(i)],
                                e.box_min[size_t(i)] - box_max[size_t(i)]});
      d2 += g * g;
    }
    if (d2 <= gap2) return true;
  }
  return false;
}

bool prop_lod_degraded(const std::array<float, 3>& near_min, const std::array<float, 3>& near_max,
                       size_t near_objects, const std::array<float, 3>& cand_min,
                       const std::array<float, 3>& cand_max, size_t cand_objects) {
  if (cand_objects < near_objects) return true;
  const float near_ext = box_max_extent(near_min, near_max);
  const float cand_ext = box_max_extent(cand_min, cand_max);
  return (near_ext - cand_ext) > 2.0f && cand_ext < near_ext * 0.85f;
}

bool TrackMaterialSettings::load_xml(const std::string& xml_text) {
  if (xml_text.empty()) return false;
  // Strip XML comments so commented-out tags are never parsed.
  std::string clean;
  clean.reserve(xml_text.size());
  for (size_t i = 0; i < xml_text.size();) {
    if (i + 3 < xml_text.size() && xml_text.compare(i, 4, "<!--") == 0) {
      const size_t end = xml_text.find("-->", i + 4);
      if (end == std::string::npos) break;
      i = end + 3;
    } else {
      clean.push_back(xml_text[i++]);
    }
  }

  auto try_any_attr = [](std::string_view tag_body, std::initializer_list<const char*> names, float& dst) -> bool {
    for (const char* n : names) {
      if (xml_attr_float(tag_body, n, dst)) return true;
    }
    return false;
  };
  auto try_child_value = [](std::string_view inner, std::initializer_list<const char*> names, float& dst) -> bool {
    if (inner.empty()) return false;
    for (const char* n : names) {
      std::string_view cb, ci;
      if (find_xml_element(inner, n, cb, ci)) {
        if (xml_attr_float(cb, "value", dst) || xml_attr_float(cb, "val", dst) || parse_finite_float(ci, dst))
          return true;
      }
    }
    return false;
  };

  bool matched_any = false;
  std::string_view tag_body, inner;
  if (find_xml_element(clean, "RoadSpecular", tag_body, inner)) {
    matched_any = true;
    float v = 0.0f;
    if (try_any_attr(tag_body, {"RoadSpec", "RoadSpecular", "Specular", "Intensity", "Scale", "value"}, v) ||
        try_child_value(inner, {"RoadSpec", "RoadSpecular", "Specular", "Intensity", "Scale"}, v)) {
      road_specular_scale = std::clamp(v, 0.0f, 4.0f);
    }
    if (try_any_attr(tag_body, {"RoadGlossiness", "Glossiness", "Power", "Exponent"}, v) ||
        try_child_value(inner, {"RoadGlossiness", "Glossiness", "Power", "Exponent"}, v)) {
      road_glossiness = std::clamp(v, 4.0f, 256.0f);
    }
    if (try_any_attr(tag_body, {"RoadFresnel", "Fresnel"}, v) ||
        try_child_value(inner, {"RoadFresnel", "Fresnel"}, v)) {
      road_fresnel = std::clamp(v, 0.0f, 1.0f);
    }
    if (try_any_attr(tag_body, {"RoadSkid", "Skid"}, v) ||
        try_child_value(inner, {"RoadSkid", "Skid"}, v)) {
      road_skid_strength = std::clamp(v, 0.0f, 4.0f);
    }
    if (try_any_attr(tag_body, {"RoadSkid2", "Skid2"}, v) ||
        try_child_value(inner, {"RoadSkid2", "Skid2"}, v)) {
      road_skid2_strength = std::clamp(v, 0.0f, 4.0f);
    }
  }
  if (find_xml_element(clean, "RoadGlossiness", tag_body, inner)) {
    matched_any = true;
    float v = 0.0f;
    if (xml_attr_float(tag_body, "value", v) || parse_finite_float(inner, v)) {
      road_glossiness = std::clamp(v, 4.0f, 256.0f);
    }
  }
  if (find_xml_element(clean, "TrackLightMapParams", tag_body, inner)) {
    matched_any = true;
    float v = 0.0f;
    if (try_any_attr(tag_body, {"Scale", "Intensity", "Gain", "Multiplier", "NightScale", "LightMapScale", "value"}, v) ||
        try_child_value(inner, {"Scale", "Intensity", "Gain", "Multiplier", "NightScale", "LightMapScale"}, v)) {
      lightmap_scale = std::clamp(v, 0.0f, 6.0f);
    }
    if (try_any_attr(tag_body, {"Bias", "Offset", "Ambient"}, v) ||
        try_child_value(inner, {"Bias", "Offset", "Ambient"}, v)) {
      lightmap_bias = std::clamp(v, 0.0f, 0.5f);
    }
    if (try_any_attr(tag_body, {"AOFloor", "MinAO", "Min", "DarkFloor"}, v) ||
        try_child_value(inner, {"AOFloor", "MinAO", "Min", "DarkFloor"}, v)) {
      lightmap_ao_floor = std::clamp(v, 0.0f, 0.5f);
    }
  }
  loaded = matched_any;
  return matched_any;
}

bool TrackMaterialSettings::load_file(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  return load_xml(ss.str());
}

ResolvedMaterialSlots resolve_effect_material(
    const std::string& effect,
    const std::array<uint32_t, 8>& slot_records,
    const std::array<uint32_t, 8>& file_slot_records,
    const std::vector<std::array<float, 4>>& pixel_constants,
    const TrackMaterialSettings& track_settings,
    uint32_t albedo_tex_id) {
  ResolvedMaterialSlots out;
  MeshMaterial& mm = out.material;
  mm.effect = effect;
  mm.blend = effect_blend(effect);
  mm.albedo = slot_records[0];

  auto constant = [&](size_t i) -> std::array<float, 4> {
    return i < pixel_constants.size() ? pixel_constants[i] : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
  };
  auto starts = [&](const char* prefix) { return mm.effect.rfind(prefix, 0) == 0; };
  auto has_tok = [&](const char* tok) { return mm.effect.find(tok) != std::string::npos; };

  if (is_water_effect(mm.effect)) {
    mm.albedo = kNoTexture;  // slot 0 on LAKE_* is a cube map, not a 2D diffuse
    mm.is_water = 1;
    mm.normal_map = file_slot_records[1];  // NormalMap A (only when file-backed)
    mm.layer_b = file_slot_records[2];     // NormalMap B / foam map (only when file-backed)
  }
  if ((has_tok("ANIM_FLAG") || has_tok("FLAG")) && !has_tok("FLAGPOLE")) {
    mm.anim_flag = kAnimFlagCloth;
  }
  if (has_tok("COUNTDOWN")) {
    // Overhead race gantry 8-row LED countdown display (0:05..0:01, GO, Horizon Festival TV).
    mm.anim_flag = kAnimCountdownLed;
    mm.blend = Blend::kOpaque;
    mm.layer_b = file_slot_records[1];
    const float mask_rep = (pixel_constants.size() > 1 && std::isfinite(pixel_constants[1][0]) &&
                            pixel_constants[1][0] > 1.0f)
                               ? std::clamp(pixel_constants[1][0], 4.0f, 128.0f)
                               : 24.0f;
    mm.tiling = {1.0f, 1.0f, mask_rep, 1.0f};
  } else if (has_tok("FLIPBOOK")) {
    // 8x2 animated LED billboard / festival TV flipbook display.
    mm.anim_flag = kAnimFlipbookLed;
    mm.blend = Blend::kOpaque;
    mm.layer_b = file_slot_records[1];
    const float mask_rep = (pixel_constants.size() > 1 && std::isfinite(pixel_constants[1][0]) &&
                            pixel_constants[1][0] > 1.0f)
                               ? std::clamp(pixel_constants[1][0], 4.0f, 128.0f)
                               : 20.0f;
    mm.tiling = {1.0f, 1.0f, mask_rep, 2.0f};
  }
  constexpr uint32_t kGrassTextureId = 0x00005007u;
  if ((mm.albedo != kNoTexture && albedo_tex_id == kGrassTextureId) || has_tok("GRASS")) {
    mm.is_grass = 1;
  }

  const bool vblend = starts("H_VBLND");
  const bool decal = starts("H_VBLND_DECAL");
  const bool blnd3 = starts("H_BLND3");
  const bool splat = starts("H_BLND2") || blnd3;
  const bool road = starts("H_ROAD");
  const bool prop_diff = starts("H_DIFF");
  const bool has_norm = has_tok("_NORM");
  const bool has_ao = has_tok("_AO");
  const bool has_lm = mm.effect.ends_with("_LM");
  const bool has_emis = has_tok("EMIS");
  const bool has_spec = has_tok("_SPEC");
  const bool has_noise = has_tok("_NOISE");
  const bool has_modulate = has_tok("_MODULATE");
  const bool has_mask = has_tok("_MASK");
  const bool has_refl = has_tok("_REFL");

  if (vblend) out.weight_gain = constant(4)[0];
  if ((vblend && !decal) || splat || starts("H_BLND1")) mm.tiling = constant(0);

  if (road) {
    mm.is_road = kRoadSurface;
    if (!pixel_constants.empty()) {
      auto c0 = constant(0);
      for (float& v : c0)
        if (std::fabs(v) < 1e-4f || !std::isfinite(v)) v = 1.0f;
      mm.tiling = c0;
    }
    const bool road_diff3 = has_tok("DIFF3");
    const bool road_diff2 = has_tok("DIFF2");
    if (road_diff2 || road_diff3) {
      const uint32_t lb = file_slot_records[1];
      if (lb != kNoTexture) {
        mm.layers = Layers::kVertexBlend;
        mm.layer_b = lb;
        if (road_diff3) {
          const uint32_t lc = file_slot_records[5];
          if (lc != kNoTexture) {
            mm.layer_c = lc;
            const auto c1 = constant(1);
            const float cx = (std::fabs(c1[0]) > 1e-4f && std::isfinite(c1[0])) ? c1[0] : mm.tiling[2];
            const float cy = (std::fabs(c1[1]) > 1e-4f && std::isfinite(c1[1])) ? c1[1] : mm.tiling[3];
            mm.tiling_c = {cx, cy};
          }
        }
      }
    }
    // H_ROAD Noise (slot 2) provides macro asphalt surface variation & specular breakup.
    if (has_noise) {
      const uint32_t noise_tex = file_slot_records[2];
      if (noise_tex != kNoTexture) {
        mm.splat = noise_tex;
        mm.is_road |= kRoadNoise;
        const auto c1 = constant(1);
        const float nx = (pixel_constants.size() > 1 && std::fabs(c1[2]) > 1e-4f && std::isfinite(c1[2])) ? c1[2] : 1.0f;
        const float ny = (pixel_constants.size() > 1 && std::fabs(c1[3]) > 1e-4f && std::isfinite(c1[3])) ? c1[3] : 1.0f;
        out.splat_uv = 1;
        out.splat_tiling = {nx, ny};
      }
    }
    // H_ROAD Modulate (slot 6) provides macro wear / lane / shoulder albedo & gloss modulation.
    if (has_modulate) {
      const uint32_t mod_tex = file_slot_records[6];
      if (mod_tex != kNoTexture) {
        mm.spec_map = mod_tex;
        mm.is_road |= kRoadModulate;
        if (out.splat_uv < 0) {
          out.splat_uv = 1;
          out.splat_tiling = {1.0f, 1.0f};
        }
      }
    }
    // Pass TrackSettings.xml RoadSpecular scale & RoadGlossiness into the road shader.
    mm.decal_noise_c4 = {track_settings.road_specular_scale, track_settings.road_glossiness};
  }

  if (decal) {
    mm.layers = Layers::kVertexFade;
    out.decal_fade = true;
    // H_VBLND_DECAL*_NOISE*: Noise_Sampler in slot 2, tiled by c1.zw, modulated by c4.xy.
    if (has_noise) {
      mm.splat = file_slot_records[2];
      const auto c1 = constant(1);
      const float nx = (std::fabs(c1[2]) > 1e-4f && std::isfinite(c1[2])) ? c1[2] : 1.0f;
      const float ny = (std::fabs(c1[3]) > 1e-4f && std::isfinite(c1[3])) ? c1[3] : 1.0f;
      mm.tiling = {1.0f, 1.0f, nx, ny};
      if (pixel_constants.size() > 4) {
        const auto c4 = constant(4);
        mm.decal_noise_c4 = {c4[0], std::max(std::fabs(c4[1]), 0.01f)};
      }
    } else if (has_mask) {
      // H_VBLND_DECAL*_MASK*: decal alpha mask texture (slot 2, fallback slot 1),
      // signaled to world_frag.glsl via negative decal_noise_c4.y.
      uint32_t mask_tex = file_slot_records[2];
      if (mask_tex == kNoTexture) mask_tex = file_slot_records[1];
      if (mask_tex != kNoTexture) {
        mm.splat = mask_tex;
        const auto c0 = constant(0);
        const auto c1 = constant(1);
        const float ax = (!pixel_constants.empty() && std::fabs(c0[0]) > 1e-4f && std::isfinite(c0[0])) ? c0[0] : 1.0f;
        const float ay = (!pixel_constants.empty() && std::fabs(c0[1]) > 1e-4f && std::isfinite(c0[1])) ? c0[1] : 1.0f;
        const float mx = (pixel_constants.size() > 1 && std::fabs(c1[2]) > 1e-4f && std::isfinite(c1[2])) ? c1[2] : 1.0f;
        const float my = (pixel_constants.size() > 1 && std::fabs(c1[3]) > 1e-4f && std::isfinite(c1[3])) ? c1[3] : 1.0f;
        mm.tiling = {ax, ay, mx, my};
        mm.decal_noise_c4 = {1.0f, -1.0f};
      }
    }
  }

  if (vblend && !decal) {
    mm.layers = Layers::kVertexBlend;
    mm.layer_b = slot_records[1];
  } else if (splat) {
    mm.layers = Layers::kSplat;
    mm.layer_b = slot_records[1];
    mm.splat = slot_records[2];
    out.splat_uv = 1;
    out.splat_tiling = {constant(1)[2], constant(1)[3]};
    if (blnd3) {
      mm.layer_c = file_slot_records[5];
      mm.tiling_c = {constant(1)[0], constant(1)[1]};
    }
  }

  if (has_norm && !mm.is_water) {
    mm.normal_map = (prop_diff || starts("ANIM_FLAG_")) ? file_slot_records[1] : file_slot_records[3];
  }

  // Restore authored AO on all _AO effects including H_VBLND_*_AO* (file_slot_records already
  // filters out type-1 all-white MIDDIST stubs so hills remain un-darkened while near terrain &
  // architecture receive full authored AO).
  if (has_ao) {
    if (road && has_tok("DIFF3") && has_modulate) {
      mm.ao_map = file_slot_records[4];
    } else if (blnd3 || (road && has_tok("DIFF3"))) {
      // H_BLND3* and H_ROAD_DIFF3* (without MODULATE) put Blend_C in slot 5 and AO in slot 6.
      mm.ao_map = file_slot_records[6];
    } else {
      mm.ao_map = file_slot_records[5];
    }
  }

  if (has_lm) {
    mm.light_map = file_slot_records[7];
    if (pixel_constants.size() > 3) mm.emissive_force = std::clamp(pixel_constants[3][3], 0.0f, 1.0f);
    if (!road && !decal && mm.layer_c == kNoTexture) {
      mm.decal_noise_c4 = {track_settings.lightmap_scale, track_settings.lightmap_ao_floor};
    }
  }
  // Reflective world/prop effects (H_DIFF_SPEC_REFL_*, e.g. Horizon Festival hub arches):
  // carry authored cubemap reflection strength c2.x via negative decal_noise_c4[1] sentinel
  // while preserving lightmap_scale in decal_noise_c4[0].
  if (has_refl && !mm.is_water && !road && !decal && !starts("H_INT_") && mm.layer_c == kNoTexture) {
    const float raw_refl = (pixel_constants.size() > 2 && std::isfinite(pixel_constants[2][0]) &&
                            pixel_constants[2][0] > 0.01f)
                               ? pixel_constants[2][0]
                               : 1.0f;
    mm.decal_noise_c4 = {track_settings.lightmap_scale, -std::clamp(raw_refl, 0.15f, 4.0f)};
  }
  // The hub rooms' interior effects (H_INT_*, uiautoshow / uiworkshop /
  // uipaintshop; Q222, decoded from their pixel shaders): c0.xy tiles the
  // diffuse, c0.z scales it and c2.x the cube reflection; NormalMap is
  // slot 1. H_INT_ROAD_DIFF2 (the paint shop's floor) blends Blend_A and
  // Blend_B by vertex colour, tiled by c0. world_frag.glsl's interior
  // branch reads (c0.z, c2.x) from decal_noise_c4.
  if (starts("H_INT_")) {
    const auto c0 = constant(0);
    auto tile = [](float v) { return (std::isfinite(v) && std::fabs(v) > 1e-4f) ? v : 1.0f; };
    if (has_tok("_ROAD_")) {
      mm.tiling = {tile(c0[0]), tile(c0[1]), tile(c0[2]), tile(c0[3])};
      if (has_tok("DIFF2") && file_slot_records[1] != kNoTexture) {
        mm.layers = Layers::kVertexBlend;
        mm.layer_b = file_slot_records[1];
      }
      mm.decal_noise_c4 = {1.0f, 0.0f};
    } else {
      mm.tiling = {tile(c0[0]), tile(c0[1]), 1.0f, 1.0f};
      const float diffuse = std::isfinite(c0[2]) ? std::max(c0[2], 0.0f) : 1.0f;
      const float reflect = std::isfinite(constant(2)[0]) ? std::max(constant(2)[0], 0.0f) : 0.0f;
      mm.decal_noise_c4 = {diffuse, has_lm ? reflect : 0.0f};
      if (has_norm) mm.normal_map = file_slot_records[1];
      // A Light_Map that is a stub record (no file; the paint booth's door
      // glass, whose c0.z of 0 leaves it black anyway) takes the AO map,
      // so the draw keeps the interior light (ours).
      if (has_lm && mm.light_map == kNoTexture && mm.ao_map != kNoTexture) mm.light_map = mm.ao_map;
    }
  }
  if (has_emis) {
    // Staggered dusk/dawn EmissiveMapSampler (slot 3 on prop emissive shaders, SKY_AND_TOD §6.2).
    // Preserve BOTH Light_Map (slot 7 -> light_map) and EmissiveMapSampler (slot 3 -> layer_c)
    // on dual _EMIS + _LM building props so night light spill and staggered lit windows both render.
    const uint32_t em = file_slot_records[3];
    if (em != kNoTexture) {
      if (mm.light_map == kNoTexture) {
        mm.light_map = em;
        mm.is_emissive_map = kEmissiveOnly;
      } else if (mm.layer_c == kNoTexture) {
        mm.layer_c = em;
        mm.is_emissive_map = kEmissiveDualLm;
      }
      if (pixel_constants.size() > 3) mm.emissive_force = std::clamp(pixel_constants[3][3], 0.0f, 1.0f);
    }
  }

  // Bind SpecularMap across both prop (H_DIFF_*_SPEC*) and non-prop world materials
  // (H_VBLND*_SPEC*, H_BLND*_SPEC*, H_ROAD*_SPEC*), never overwriting H_ROAD Modulate or AO.
  const bool slot4_is_ao = (road && has_tok("DIFF3") && has_modulate);
  if (has_spec && mm.spec_map == kNoTexture && !mm.is_water) {
    uint32_t sp = slot4_is_ao ? kNoTexture : file_slot_records[4];
    if (sp == kNoTexture && starts("H_BLND1") && !has_norm && !has_tok("_REFL")) {
      const uint32_t cand = file_slot_records[1];
      if (cand != kNoTexture && cand != mm.ao_map && cand != mm.normal_map) sp = cand;
    }
    mm.spec_map = sp;
  } else if (has_tok("_NORM2") && !has_spec && !slot4_is_ao && mm.spec_map == kNoTexture && !mm.is_water &&
             mm.layer_b != kNoTexture) {
    // Multi-layer _NORM2 road/terrain effects (H_ROAD_DIFF2/3_NORM2*, H_BLND2/3_NORM2*, H_VBLND_NORM2*)
    // store Layer B's DXN/BC5 normal map in RMB slot 4; carry it in spec_map for Layer B normal blending.
    const uint32_t nb = file_slot_records[4];
    if (nb != kNoTexture && nb != mm.ao_map && nb != mm.normal_map) {
      mm.spec_map = nb;
    }
  }

  return out;
}

const char* surface_name(Surface s) {
  switch (s) {
    case Surface::kRoad: return "road";
    case Surface::kTerrain: return "terrain";
    case Surface::kWater: return "water";
    case Surface::kOther: return "other";
    case Surface::kProp: return "prop";
  }
  return "?";
}

bool skipped_object(const std::string& object) {
  const std::string o = lower(object);
  return has(o, "terr_cube") || has(o, "uberlod") || has(o, "middist") || has(o, "occluder") ||
         has(o, "underground") || has(o, "_cage") || has(o, "shadowmountain") || has(o, "shadowproxy") ||
         has(o, "shadowcaster");
}

// The far ring's level of an object: 1 MIDDIST, 2 UberLOD, 0 neither.
// TERR_CUBE objects are not scenery (ours: road and diffuse effects over
// the terrain's footprint, by their name the cube maps' stand-ins).
uint8_t far_level(const std::string& object, const std::vector<std::string>& effects) {
  const std::string o = lower(object);
  if (has(o, "terr_cube") || has(o, "occluder") || has(o, "cage") || has(o, "underground") ||
      has(o, "shadowmountain") || has(o, "shadowproxy") || has(o, "shadowcaster"))
    return 0;
  if (has(o, "uberlod")) return 2;
  if (has(o, "middist")) return 1;
  // By effect (ours): H_BLND1_SPEC is the UberLOD patches' effect
  // (WORLD-MATERIALS, Terrain shading), drawn by nothing but the far
  // terrain. An object drawn only with it is an UberLOD patch whatever its
  // name: Mountains_Area04A_Terrain_05_LOD00..02, a 4 x 3.5 km sheet in the
  // UberLOD block of the members (between Patch10 and its CAGE003), lies
  // 1 to 2 m over the Finley Dam roads and hills it covers.
  if (effects.empty()) return 0;
  for (const auto& e : effects)
    if (e != "H_BLND1_SPEC") return 0;
  return 2;
}

GroundCover::GroundCover(double x, double z, double half_width, double cell)
    : x0_(x - half_width), z0_(z - half_width), cell_(cell > 0 ? cell : 1),
      side_(std::max(1, int(std::ceil(2 * half_width / (cell > 0 ? cell : 1))))) {
  const size_t n = size_t(side_) * size_t(side_);
  covered_.assign(n, 0);
  max_y_.assign(n, -1e30f);
}

long GroundCover::cell_of(double x, double z) const {
  const double i = std::floor((x - x0_) / cell_), j = std::floor((z - z0_) / cell_);
  if (i < 0 || j < 0 || i >= side_ || j >= side_) return -1;
  return long(j) * side_ + long(i);
}

bool GroundCover::covered(double x, double z) const {
  const long c = cell_of(x, z);
  return c < 0 || covered_[size_t(c)] != 0;
}

bool GroundCover::covered_at(double x, double y, double z, double margin) const {
  const long c = cell_of(x, z);
  if (c < 0) return true;
  if (covered_[size_t(c)] == 0) return false;
  return y <= double(max_y_[size_t(c)]) + margin;
}

void GroundCover::add(const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c) {
  const double d = (double(b[2]) - c[2]) * (double(a[0]) - c[0]) + (double(c[0]) - b[0]) * (double(a[2]) - c[2]);
  if (std::fabs(d) < 1e-9) return;  // edge-on (armco, poles): no footprint
  const double x_lo = std::min({a[0], b[0], c[0]}), x_hi = std::max({a[0], b[0], c[0]});
  const double z_lo = std::min({a[2], b[2], c[2]}), z_hi = std::max({a[2], b[2], c[2]});
  const int i0 = std::max(0, int(std::ceil((x_lo - x0_) / cell_ - 0.5)));
  const int i1 = std::min(side_ - 1, int(std::floor((x_hi - x0_) / cell_ - 0.5)));
  const int j0 = std::max(0, int(std::ceil((z_lo - z0_) / cell_ - 0.5)));
  const int j1 = std::min(side_ - 1, int(std::floor((z_hi - z0_) / cell_ - 0.5)));
  bool any = false;
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      const double px = x0_ + (i + 0.5) * cell_, pz = z0_ + (j + 0.5) * cell_;
      const double l1 = ((double(b[2]) - c[2]) * (px - c[0]) + (double(c[0]) - b[0]) * (pz - c[2])) / d;
      const double l2 = ((double(c[2]) - a[2]) * (px - c[0]) + (double(a[0]) - c[0]) * (pz - c[2])) / d;
      const double l3 = 1.0 - l1 - l2;
      if (l1 < 0 || l2 < 0 || l3 < 0) continue;
      const size_t idx = size_t(j) * size_t(side_) + size_t(i);
      const float py = float(l1 * a[1] + l2 * b[1] + l3 * c[1]);
      covered_[idx] = 1;
      if (py > max_y_[idx]) max_y_[idx] = py;
      any = true;
    }
  if (!any) {
    const long cc = cell_of((double(a[0]) + b[0] + c[0]) / 3, (double(a[2]) + b[2] + c[2]) / 3);
    if (cc >= 0) {
      const float py = std::max({a[1], b[1], c[1]});
      covered_[size_t(cc)] = 1;
      if (py > max_y_[size_t(cc)]) max_y_[size_t(cc)] = py;
    }
  }
}

std::array<std::array<double, 2>, 7> footprint_samples(const std::array<float, 3>& a, const std::array<float, 3>& b,
                                                       const std::array<float, 3>& c) {
  auto mid = [](const std::array<float, 3>& p, const std::array<float, 3>& q) {
    return std::array<double, 2>{(double(p[0]) + q[0]) / 2, (double(p[2]) + q[2]) / 2};
  };
  return {{{a[0], a[2]},
           {b[0], b[2]},
           {c[0], c[2]},
           mid(a, b),
           mid(b, c),
           mid(c, a),
           {(double(a[0]) + b[0] + c[0]) / 3, (double(a[2]) + b[2] + c[2]) / 3}}};
}

std::array<std::array<double, 3>, 7> footprint_samples_3d(const std::array<float, 3>& a, const std::array<float, 3>& b,
                                                          const std::array<float, 3>& c) {
  auto mid = [](const std::array<float, 3>& p, const std::array<float, 3>& q) {
    return std::array<double, 3>{(double(p[0]) + q[0]) / 2, (double(p[1]) + q[1]) / 2, (double(p[2]) + q[2]) / 2};
  };
  return {{{a[0], a[1], a[2]},
           {b[0], b[1], b[2]},
           {c[0], c[1], c[2]},
           mid(a, b),
           mid(b, c),
           mid(c, a),
           {(double(a[0]) + b[0] + c[0]) / 3, (double(a[1]) + b[1] + c[1]) / 3, (double(a[2]) + b[2] + c[2]) / 3}}};
}

namespace {

// An object's effect per submesh (upper case); empty without the model's
// materials (parse_render_materials) or with a submesh that has none.
std::vector<std::string> object_effects(const fh1::RenderModel& model, const fh1::RmbObject& o) {
  std::vector<std::string> effects;
  for (const auto& s : o.submeshes) {
    if (s.material_index >= model.materials.size()) return {};
    const uint32_t e = model.materials[s.material_index].effect;
    if (e >= model.effects.size()) return {};
    effects.push_back(model.effects[e]);
  }
  return effects;
}

}  // namespace

bool skipped_material(const std::string& material) {
  const std::string m = lower(material);
  return m == "occluder" || m == "underground" || m == "crowdterr";
}

bool ground_object(const std::string& object) {
  const std::string o = lower(object);
  for (const char* part : {"armco", "barrier", "railing", "cable", "tether", "gate", "fence", "wall", "truck",
                           "flag", "banner", "sign", "pole"})
    if (has(o, part)) return false;
  return true;
}

Surface classify(const std::string& object, const std::string& material) {
  const std::string o = lower(object);
  if (has(o, "river") || has(o, "water") || has(o, "lake")) return Surface::kWater;
  if (has(o, "_road") || has(o, "road_")) return Surface::kRoad;
  if (has(o, "terr") || has(o, "rocks") || has(o, "patch")) return Surface::kTerrain;
  const std::string m = lower(material);
  if (has(m, "water") || has(m, "river") || has(m, "lake")) return Surface::kWater;
  if (has(m, "road") || has(m, "asphalt") || has(m, "tarmac") || has(m, "concrete") || has(m, "junction") ||
      has(m, "carpark") || has(m, "car_park") || has(m, "parking"))
    return Surface::kRoad;
  if (has(m, "terrain") || has(m, "grass") || has(m, "dirt") || has(m, "gravel") || has(m, "ground") ||
      has(m, "rock") || has(m, "sand") || has(m, "mud") || has(m, "verge"))
    return Surface::kTerrain;
  return Surface::kOther;
}

struct World::Impl {
  fh1::MemberSource source;
  // The `.bundle` members' entries by texture record (WORLD-MATERIALS,
  // Small textures), read on the first bundle-only record. A record in
  // several bundles has the same payload in all: the first is kept.
  struct Small {
    uint32_t width = 0, height = 0, levels = 0, format_word = 0;
    std::vector<uint8_t> payload;
  };
  std::mutex small_mutex;
  bool small_read = false;
  std::unordered_map<uint32_t, Small> small;
  void read_small() {
    if (small_read) return;
    small_read = true;
    const auto start = std::chrono::steady_clock::now();
    const auto names = source.with_suffix(".bundle");
    struct BundleResult {
      std::vector<uint8_t> data;
      std::vector<fh1::BundleEntry> entries;
      bool ok = false;
    };
    std::vector<BundleResult> results(names.size());
    const size_t threads = std::min<size_t>(names.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
    if (threads > 1 && names.size() >= 4) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= names.size()) break;
            auto& r = results[i];
            if (source.read(names[i], r.data, nullptr) && fh1::parse_bundle(r.data, r.entries, nullptr)) {
              r.ok = true;
            }
          }
        });
      }
      for (auto& w : workers) w.join();
    } else {
      for (size_t i = 0; i < names.size(); ++i) {
        auto& r = results[i];
        if (source.read(names[i], r.data, nullptr) && fh1::parse_bundle(r.data, r.entries, nullptr)) r.ok = true;
      }
    }
    for (auto& r : results) {
      if (!r.ok) continue;
      for (const auto& e : r.entries)
        if (!small.count(e.record))
          small[e.record] = {e.width, e.height, e.levels, e.format_word, {e.payload.begin(), e.payload.end()}};
    }
    if (std::getenv("SHIFT_WORLD_DEBUG"))
      std::fprintf(stderr, "world: %zu bundle textures in %.0f ms\n", small.size(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  fh1::PvsObjectTable table;
  fh1::PropWorld props;
  std::unique_ptr<fh1::PropModels> prop_models;
  TrackMaterialSettings track_settings;
  bool have_props = false;
};

World::World() : impl_(std::make_unique<Impl>()) {}
World::~World() = default;

size_t World::placement_count() const { return impl_->have_props ? impl_->props.placements.size() : 0; }

const std::vector<fh1::PvsTexture>& World::textures() const { return impl_->table.textures; }

const TrackMaterialSettings& World::track_settings() const { return impl_->track_settings; }

bool World::read_texture(uint32_t record, fh1::WorldTexture& out, std::string* error) {
  if (record >= impl_->table.textures.size()) {
    if (error) *error = "no texture record " + std::to_string(record);
    return false;
  }
  const fh1::PvsTexture& t = impl_->table.textures[record];
  char name[32];
  std::vector<uint8_t> a, b;
  bool exact = false, ok = false;
  if (t.type == fh1::kPvsTextureBix) {
    std::snprintf(name, sizeof name, "_0x%08x.bix", t.id);
    std::string base = name;
    base.insert(base.size() - 4, "_b");
    ok = impl_->source.read(name, a, error) && impl_->source.read(base, b, error) &&
         fh1::decode_bix(a, b, out, error, &exact);
  } else if (t.type == fh1::kPvsTextureCaff) {
    std::snprintf(name, sizeof name, "_0x%08x.bin", t.id);
    ok = impl_->source.read(name, a, error) && fh1::decode_caff(a, out, error, &exact);
  } else {
    // Types 1 and 3: the bundle entry is the only copy.
    std::lock_guard<std::mutex> lock(impl_->small_mutex);
    impl_->read_small();
    const auto it = impl_->small.find(record);
    if (it == impl_->small.end()) {
      if (error) *error = "texture record " + std::to_string(record) + " is in no bundle";
      return false;
    }
    const auto& s = it->second;
    return fh1::decode_bundle_entry({record, s.width, s.height, s.levels, s.format_word, s.payload}, out, error);
  }
  if (ok && !exact) {
    if (error) *error = std::string(name) + ": sizes do not fit the layout";
    return false;
  }
  return ok;
}

bool is_water_effect(std::string_view effect) {
  if (effect.empty()) return false;
  std::string up;
  up.reserve(effect.size());
  for (char c : effect) up.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  if (up.rfind("LAKE", 0) == 0 || up.find("_LAKE") != std::string::npos) return true;
  if (up.find("RIVER") != std::string::npos || up.find("POND") != std::string::npos) return true;
  if (up.find("WATER") != std::string::npos && up.find("WATERMARK") == std::string::npos) return true;
  return false;
}

bool decal_effect(const std::string& e) {
  if (e.find("DECAL") != std::string::npos || e.find("_BIAS") != std::string::npos) return true;
  return effect_blend(e) == Blend::kAlpha && !is_water_effect(e);
}

Blend effect_blend(const std::string& e) {
  auto starts = [&](const char* prefix) { return e.rfind(prefix, 0) == 0; };
  if (starts("DIFF_GLOW") || starts("ADD_")) return Blend::kAdditive;
  if ((starts("H_ROAD") && e.find("_BLEND") != std::string::npos) || starts("H_VBLND_DECAL") || is_water_effect(e))
    return Blend::kAlpha;
  // Props, trees, curved cloth flags (ANIM_FLAG_*, excluding rigid ANIM_FLAGPOLE_*),
  // bunting banners, and _OPAC cut-outs draw two-sided with alpha to coverage.
  if (starts("H_DIFF") || starts("TREE_") || (starts("ANIM_FLAG_") && !starts("ANIM_FLAGPOLE")) ||
      starts("BUNTING_") || e.find("_OPAC") != std::string::npos)
    return Blend::kCoverage;
  return Blend::kOpaque;
}

fs::path ribbon_file(const fs::path& world, const std::string& suffix) {
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const fs::path ribbon = world / "Ribbon_00";
  const std::string name = (world.has_filename() ? world : world.parent_path()).filename().string();
  const std::string want = lower(name + suffix);
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(ribbon, ec))
    if (lower(f.path().filename().string()) == want) return f.path();
  return ribbon / (name + suffix);
}

std::string model_prefix(const fs::path& world) {
  std::string s = (world.has_filename() ? world : world.parent_path()).filename().string();
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s + "out";
}

bool World::open(const fs::path& colorado, const fs::path& cache, std::string* error) {
  name_ = (colorado.has_filename() ? colorado : colorado.parent_path()).filename().string();
  std::error_code ec;
  fs::path zip = colorado / "bin.zip";
  fs::path dlc_zips;
  if (!fs::exists(zip, ec)) {
    // Horizon Rally DLC package layout:
    // <pkg>/Media/DLCZips/ColoradoDirt_pri_65.zip alongside 1600_pri_65/Media/Tracks/ColoradoDirt.
    const fs::path alt1 = colorado / "ColoradoDirt_pri_65.zip";
    const fs::path parent_zips = colorado.parent_path().parent_path().parent_path().parent_path();
    const fs::path alt2 = parent_zips / "ColoradoDirt_pri_65.zip";
    if (fs::exists(alt1, ec)) {
      zip = alt1;
      dlc_zips = colorado;
    } else if (fs::exists(alt2, ec)) {
      zip = alt2;
      dlc_zips = parent_zips;
    }
  }
  if (!impl_->source.open(zip, error)) return false;
  const auto zip_size = fs::file_size(zip, ec);

  // The model index: the cache, or a parse of every model.
  models_.clear();
  bool cached = false;
  if (!cache.empty()) {
    std::ifstream in(cache);
    std::string magic;
    unsigned long long size = 0;
    if (std::getline(in, magic) && magic == kCacheMagic && (in >> size) && size == zip_size) {
      ModelInfo m;
      int ws = 0, ok = 0, far = 0;
      while (in >> m.name >> m.box_min[0] >> m.box_min[1] >> m.box_min[2] >> m.box_max[0] >> m.box_max[1] >>
             m.box_max[2] >> ws >> ok >> far) {
        m.world_space = ws != 0;
        m.ok = ok != 0;
        m.far = far != 0;
        models_.push_back(m);
      }
      cached = !models_.empty();
    }
  }
  if (!cached) {
    models_.clear();
    std::vector<uint8_t> bytes;
    for (const auto& name : impl_->source.with_suffix(".rmb.bin")) {
      ModelInfo m;
      m.name = name;
      fh1::RenderModel model;
      std::string e;
      if (impl_->source.read(name, bytes, &e) && fh1::parse_render_model(bytes, model, &e)) {
        m.ok = true;
        m.box_min = model.box_min;
        m.box_max = model.box_max;
        m.world_space = fh1::rmb_is_world_space(model);
        std::string ignored;
        fh1::parse_render_materials(bytes, model, &ignored);  // the effects, for far_level
        for (const auto& o : model.objects) m.far = m.far || far_level(o.name, object_effects(model, o)) != 0;
      }
      models_.push_back(m);
    }
    if (!cache.empty()) {
      fs::create_directories(cache.parent_path(), ec);
      std::ofstream out(cache, std::ios::binary);
      out << kCacheMagic << "\n" << zip_size << "\n";
      char line[256];
      for (const auto& m : models_) {
        std::snprintf(line, sizeof line, "%s %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d\n", m.name.c_str(),
                      m.box_min[0], m.box_min[1], m.box_min[2], m.box_max[0], m.box_max[1], m.box_max[2],
                      int(m.world_space), int(m.ok), int(m.far));
        out << line;
      }
    }
  }

  // Props: the object table and every placement.
  const fs::path pvs = ribbon_file(colorado, "_00.pvs");
  std::vector<uint8_t> pvs_bytes;
  if (std::ifstream f(pvs, std::ios::binary); f) {
    pvs_bytes.assign((std::istreambuf_iterator<char>(f)), {});
  } else if (!dlc_zips.empty()) {
    std::string zwhy;
    if (auto pkg_zip = fh1::Archive::open(dlc_zips / "1600_pri_65.zip", &zwhy)) {
      auto r = pkg_zip->read("media/Tracks/ColoradoDirt/Ribbon_00/ColoradoDirt_00.pvs");
      if (r.ok()) pvs_bytes = std::move(r.data);
    }
  }
  if (!pvs_bytes.empty()) {
    fh1::PropLoadOptions options;
    options.census_first_rule = false;
    std::string e;
    if (fh1::parse_object_table(pvs_bytes, impl_->table, &e) &&
        fh1::load_props(impl_->source, impl_->props, &e, options)) {
      impl_->prop_models = std::make_unique<fh1::PropModels>(impl_->source, model_prefix(colorado));
      impl_->have_props = true;
    } else if (error) {
      *error = "props: " + e;  // not fatal: the caller may go on without props
    }
  }
  // Track-wide material settings (RoadSpecular / TrackLightMapParams from TrackSettings.xml).
  impl_->track_settings = {};
  if (!impl_->track_settings.load_file(colorado / "TrackSettings.xml") && !dlc_zips.empty()) {
    std::string zwhy;
    if (auto pkg_zip = fh1::Archive::open(dlc_zips / "1600_pri_65.zip", &zwhy)) {
      auto r = pkg_zip->read("media/Tracks/ColoradoDirt/TrackSettings.xml");
      if (r.ok()) {
        std::string xml(reinterpret_cast<const char*>(r.data.data()), r.data.size());
        impl_->track_settings.load_xml(xml);
      }
    }
  }
  return true;
}

bool World::gather(double x, double z, const GatherOptions& options, Mesh& out, GatherStats* stats,
                   std::string* error) {
  GatherStats local;
  GatherStats& st = stats ? *stats : local;
  out = {};

  std::map<decltype(MeshMaterial{}.key()), uint32_t> material_index;
  // The far ring's hole filling (ours): within its inner cut a far triangle
  // is drawn only over a hole in the near ground (a footprint cell no near
  // triangle but water covers), by the finest far object there (the least
  // mean triangle area), so the far terrain still floors what the near
  // models leave open (the gorge below the Finley Dam road, which only
  // the UberLOD-effect sheet covers) without lying over the near terrain
  // it copies. Built before the far pass: cover, and per hole cell its
  // filler (mean area, object key).
  std::unique_ptr<GroundCover> cover;
  std::unordered_map<long, std::tuple<uint8_t, double, uint64_t>> hole_fill;
  auto object_key = [](uint32_t model_index, const fh1::RenderModel& model, const fh1::RmbObject& o) {
    return (uint64_t(model_index) << 20) | uint64_t(&o - model.objects.data());
  };
  auto fills_hole = [&](uint32_t model_index, const fh1::RmbObject& o, const fh1::RenderModel& model,
                        const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c) {
    if (!cover) return false;
    const uint64_t key = object_key(model_index, model, o);
    for (const auto& s : footprint_samples_3d(a, b, c)) {
      if (cover->covered_at(s[0], s[1], s[2], 1.5)) continue;
      auto it = hole_fill.find(cover->cell_of(s[0], s[2]));
      if (it != hole_fill.end() && std::get<2>(it->second) == key) return true;
    }
    return false;
  };
  // far_pass: the far ring's objects only (the near pass skips them).
  auto add_triangles = [&](const fh1::RenderModel& model, uint32_t model_index, const fh1::Placement* placement,
                           const WorldLodFilter* lowest, bool far_pass = false, uint8_t fade = 0) {
    // The model's texture list (WORLD-MATERIALS): list j for member j + 1.
    const std::vector<uint32_t>* texture_list = nullptr;
    if (options.render_data && model_index < models_.size()) {
      const std::string& name = models_[model_index].name;
      const size_t dot = name.find('.');  // <prefix>.NNNNN.rmb.bin
      const unsigned long member = dot != std::string::npos ? std::strtoul(name.c_str() + dot + 1, nullptr, 10) : 0;
      if (member >= 1 && member - 1 < impl_->table.model_textures.size())
        texture_list = &impl_->table.model_textures[member - 1];
      if (model.uv_offset_scale.empty()) st.default_uv_models.insert(name);
    }
    size_t submesh_number = 0;  // in file order, for uvOffsetScale
    // FH1 RMB command buffers emit uvOffsetScale (c0) draws grouped by render bucket:
    // opaque/cutout submeshes first, then alpha/additive-blended submeshes second.
    // On multi-object models whose objects have distinct UV scales (e.g. road tiles
    // mixing H_ROAD_* opaque and H_ROAD_*_BLEND_* submeshes), file-order indexing
    // assigns the wrong object's uvOffsetScale and tiles AO/Light_Map across the mesh.
    std::vector<size_t> uv_draw_index;
    if (options.render_data && !model.uv_offset_scale.empty()) {
      size_t total_subs = 0;
      for (const auto& obj : model.objects) total_subs += obj.submeshes.size();
      uv_draw_index.resize(total_subs, 0);
      size_t cursor = 0;
      for (int pass = 0; pass < 2; ++pass) {
        size_t g = 0;
        for (const auto& obj : model.objects) {
          for (const auto& sub : obj.submeshes) {
            bool is_blended = false;
            if (sub.material_index < model.materials.size()) {
              const auto& mat_ref = model.materials[sub.material_index];
              if (mat_ref.effect < model.effects.size()) {
                const Blend b = effect_blend(model.effects[mat_ref.effect]);
                is_blended = (b == Blend::kAlpha || b == Blend::kAdditive);
              }
            }
            if ((pass == 1) == is_blended) uv_draw_index[g] = cursor++;
            ++g;
          }
        }
      }
    }
    std::array<float, 3> instance{0, 0, 0};
    if (placement) {
      const auto q = placement->apply({0, 0, 0});
      instance = {float(q[0]), float(q[1]), float(-q[2])};
    }
    for (const auto& o : model.objects) {
      const size_t first_submesh = submesh_number;
      submesh_number += o.submeshes.size();
      // World-space objects: the far ring's by name or by effect, which the
      // near pass leaves out with the other skipped objects.
      const uint8_t level = placement ? 0 : far_level(o.name, object_effects(model, o));
      const uint8_t far = far_pass ? level : 0;
      if (far_pass ? far == 0 : !placement && (skipped_object(o.name) || level != 0)) {
        if (!far_pass) ++st.skipped_far;
        continue;
      }
      // The far ring leaves out triangles wholly near the point.
      const double inner = far == 2 ? options.far_inner_uberlod : options.far_inner;
      if (lowest) {
        int lod;
        const std::string family = lod_family(o.name, lod);
        if (lod >= 0) {
          std::array<float, 3> bmin, bmax;
          object_bounds(o, model, bmin, bmax);
          if (lowest->suppressed(family, lod, bmin, bmax)) {
            ++st.skipped_lod;
            continue;
          }
        }
      }
      ++st.objects;
      // File frame to the game frame: z negated.
      auto position = [&](uint32_t i) {
        const auto p = o.position(i);
        if (!placement) return std::array<float, 3>{p[0], p[1], -p[2]};
        const auto q = placement->apply(p);
        return std::array<float, 3>{float(q[0]), float(q[1]), float(-q[2])};
      };
      for (size_t si = 0; si < o.submeshes.size(); ++si) {
        const auto& s = o.submeshes[si];
        if (skipped_material(s.material)) continue;
        const uint8_t solid = !placement && !far && ground_object(o.name);
        const Surface surface = placement ? Surface::kProp : far ? Surface::kTerrain : classify(o.name, s.material);
        const auto tri = fh1::rmb_triangle_list(s);
        // The render data: the material, the vertex layout and the UV scale.
        uint32_t mat = kNoTexture;
        fh1::RmbVertexLayout layout;
        std::array<float, 4> uv{0, 0, 1, 1};
        int lightmap = -1;
        int splat_uv = -1;          // kSplat / H_ROAD Noise+Modulate: TEXCOORD1, times splat_tiling
        std::array<float, 2> splat_tiling{1, 1};
        float weight_gain = -1;     // H_VBLND: c4.x of the vertex-colour weight
        bool decal_fade = false;    // H_VBLND_DECAL: the weight is the fade, from COLOR.w
        bool is_road_mat = false;
        if (options.render_data) {
          if (s.material_index < model.materials.size()) {
            const fh1::RmbMaterial& m = model.materials[s.material_index];
            const std::string& eff = model.effects[m.effect];
            if (eff == "UNDERGROUND") continue;
            // A slot's texture record: a file (types 4 and 0) or a bundle
            // entry (types 1 and 3; WORLD-MATERIALS, Small textures).
            auto slot_record = [&](size_t slot) {
              const int32_t v = slot < m.slots.size() ? m.slots[slot] : -2;
              if (v < 0 || !texture_list || size_t(v) >= texture_list->size()) return kNoTexture;
              const uint32_t record = (*texture_list)[size_t(v)];
              return record < impl_->table.textures.size() ? record : kNoTexture;
            };
            // Helper: file-backed texture record only (skips all-black Light_Map / all-white AO type-1 stubs).
            auto file_slot = [&](size_t slot) -> uint32_t {
              const uint32_t rec = slot_record(slot);
              if (rec == kNoTexture) return kNoTexture;
              const uint8_t ty = impl_->table.textures[rec].type;
              return (ty == fh1::kPvsTextureBix || ty == fh1::kPvsTextureCaff) ? rec : kNoTexture;
            };
            std::array<uint32_t, 8> raw_slots{};
            std::array<uint32_t, 8> file_slots{};
            for (size_t k = 0; k < 8; ++k) {
              raw_slots[k] = slot_record(k);
              file_slots[k] = file_slot(k);
            }
            // WORLD-MATERIALS, Placeholder models: slot 0 on the art tools'
            // missing-material marker (_0x00002CA9) is never drawn, in any
            // pass (shadow stand-ins too, interim).
            if (raw_slots[0] != kNoTexture && impl_->table.textures[raw_slots[0]].id == kPlaceholderTextureId)
              continue;
            const uint32_t albedo_id = raw_slots[0] != kNoTexture ? impl_->table.textures[raw_slots[0]].id : 0u;
            ResolvedMaterialSlots resolved = resolve_effect_material(
                eff, raw_slots, file_slots, m.pixel_constants, impl_->track_settings, albedo_id);
            MeshMaterial& mm = resolved.material;
            mm.far = far;
            mm.fade = fade;
            for (size_t k = 0; k < 3; ++k) {
              const int32_t v = k < m.slots.size() ? m.slots[k] : -2;
              mm.slot_values[k] = v;
              if (v >= 0 && texture_list && size_t(v) < texture_list->size() &&
                  (*texture_list)[size_t(v)] < impl_->table.textures.size())
                mm.slot_types[k] = int32_t(impl_->table.textures[(*texture_list)[size_t(v)]].type);
            }
            splat_uv = resolved.splat_uv;
            splat_tiling = resolved.splat_tiling;
            weight_gain = resolved.weight_gain;
            decal_fade = resolved.decal_fade;
            is_road_mat = (mm.is_road != 0u);
            auto [it, fresh] = material_index.emplace(mm.key(), uint32_t(out.materials.size()));
            if (fresh) out.materials.push_back(mm);
            mat = it->second;
            layout = fh1::rmb_vertex_layout(mm.effect, o.stride);
            if (mm.effect.ends_with("_LM") || mm.effect.find("_AO") != std::string::npos)
              lightmap = layout.last_texcoord();
            const size_t uv_idx = (first_submesh + si < uv_draw_index.size())
                                      ? uv_draw_index[first_submesh + si]
                                      : (first_submesh + si);
            if (uv_idx < model.uv_offset_scale.size()) uv = model.uv_offset_scale[uv_idx];
          } else {
            ++st.no_material;
            MeshMaterial none;
            none.far = far;
            none.fade = fade;
            auto [it, fresh] = material_index.emplace(none.key(), uint32_t(out.materials.size()));
            if (fresh) out.materials.push_back(none);
            mat = it->second;
          }
        }
        auto corner = [&](uint32_t i) {
          out.positions.push_back(position(i));
          if (!options.render_data) return;
          const uint8_t* v = &o.vertices[size_t(i) * o.stride];
          std::array<float, 3> n{0, 0, 0};
          if (layout.normal >= 0) {
            const auto f = fh1::rmb_normal(v + layout.normal);
            std::array<double, 3> g{f[0], f[1], f[2]};
            if (placement) {
              const auto& l = placement->linear;
              for (int r = 0; r < 3; ++r) g[size_t(r)] = l[size_t(r)][0] * f[0] + l[size_t(r)][1] * f[1] + l[size_t(r)][2] * f[2];
            }
            const double len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            if (len > 0) n = {float(g[0] / len), float(g[1] / len), float(-g[2] / len)};
          }
          out.normals.push_back(n);
          auto texcoord = [&](int k) {
            if (k < 0 || layout.texcoord[size_t(k)] < 0) return std::array<float, 2>{0, 0};
            const auto t = fh1::rmb_texcoord(v + layout.texcoord[size_t(k)]);
            return std::array<float, 2>{uv[0] + uv[2] * t[0], uv[1] + uv[3] * t[1]};
          };
          out.uv0.push_back(texcoord(0));
          const auto lm_uv = texcoord(lightmap);
          if (splat_uv >= 0) {
            const int tc_idx = (splat_uv == 1 && layout.texcoord[1] >= 0 && (!is_road_mat || lightmap != 1)) ? 1 : 0;
            const auto s = texcoord(tc_idx);
            out.uv1.push_back({s[0] * splat_tiling[0], s[1] * splat_tiling[1]});
          } else {
            out.uv1.push_back(lm_uv);
          }
          out.uv2.push_back(lm_uv);
          auto c = layout.colour >= 0 ? fh1::rmb_colour(v + layout.colour)
                                      : std::array<uint8_t, 4>{255, 255, 255, 255};
          if (weight_gain >= 0) {
            // WORLD-MATERIALS, Colour byte order: the layer weight is
            // COLOR.x, the second stored byte (rmb_colour's R); the decal
            // fade is COLOR.w, the first (rmb_colour's A). The decal's
            // noise term is left out (ours).
            const uint8_t byte = decal_fade ? c[3] : c[0];
            const double w = std::clamp((byte / 255.0 - 0.5) * weight_gain + 0.5, 0.0, 1.0);
            c[0] = uint8_t(std::lround(w * 255));
          }
          out.colours.push_back(c);
        };
        bool flip_winding = false;
        if (placement) {
          const auto& L = placement->linear;
          const double det = L[0][0] * (L[1][1] * L[2][2] - L[1][2] * L[2][1]) -
                             L[0][1] * (L[1][0] * L[2][2] - L[1][2] * L[2][0]) +
                             L[0][2] * (L[1][0] * L[2][1] - L[1][1] * L[2][0]);
          flip_winding = det < 0.0;
        }
        size_t added = 0;
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
          if (far) {
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) {
              const auto p = position(tri[t + size_t(k)]);
              inside = std::hypot(p[0] - x, p[2] + z) < inner;  // z is the file frame's here
            }
            if (inside && !fills_hole(model_index, o, model, position(tri[t]), position(tri[t + 1]),
                                      position(tri[t + 2])))
              continue;
            st.far_fill_triangles += inside;
            ++st.far_triangles;
          }
          corner(tri[t]);
          corner(tri[t + (flip_winding ? 2u : 1u)]);
          corner(tri[t + (flip_winding ? 1u : 2u)]);
          if (options.render_data) out.material.push_back(mat), out.instance.push_back(instance);
          out.surface.push_back(surface);
          out.model.push_back(model_index);
          out.ground.push_back(solid);
          ++added;
        }
        st.materials[s.material] += added;
        if (!placement) st.object_names[o.name] += added;
      }
    }
  };

  // The point in the file frame, where the boxes and placements are.
  z = -z;
  // Helper: read and parse a list of model indices in parallel, preserving exact order.
  auto load_models_parallel = [&](const std::vector<uint32_t>& ids, bool parse_materials,
                                  std::vector<std::pair<uint32_t, fh1::RenderModel>>& dst) -> bool {
    dst.resize(ids.size());
    if (ids.empty()) return true;
    std::vector<std::string> errs(ids.size());
    std::atomic<bool> failed{false};
    const size_t threads = std::min<size_t>(ids.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
    auto run_one = [&](size_t slot, std::vector<uint8_t>& buf) {
      const uint32_t idx = ids[slot];
      const ModelInfo& m = models_[idx];
      fh1::RenderModel model;
      std::string e;
      if (!impl_->source.read(m.name, buf, &e) || !fh1::parse_render_model(buf, model, &e)) {
        errs[slot] = m.name + ": " + e;
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      if (parse_materials) {
        std::string ignored;
        fh1::parse_render_materials(buf, model, &ignored);  // without them the model draws flat
      }
      dst[slot] = {idx, std::move(model)};
    };
    if (threads > 1 && ids.size() >= 4) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          std::vector<uint8_t> buf;
          for (;;) {
            if (failed.load(std::memory_order_relaxed)) break;
            const size_t slot = next.fetch_add(1, std::memory_order_relaxed);
            if (slot >= ids.size()) break;
            run_one(slot, buf);
          }
        });
      }
      for (auto& w : workers) w.join();
    } else {
      std::vector<uint8_t> buf;
      for (size_t slot = 0; slot < ids.size(); ++slot) {
        run_one(slot, buf);
        if (failed.load(std::memory_order_relaxed)) break;
      }
    }
    if (failed.load(std::memory_order_relaxed)) {
      if (error) {
        for (const auto& e : errs) {
          if (!e.empty()) {
            *error = e;
            break;
          }
        }
      }
      return false;
    }
    return true;
  };

  // World-space models whose header box comes within the radius.
  std::vector<uint32_t> near_ids;
  const double r = options.radius;
  for (uint32_t i = 0; i < models_.size(); ++i) {
    const ModelInfo& m = models_[i];
    if (!m.ok || (!m.world_space && !options.local_models)) continue;
    const double dx = std::max({0.0, double(m.box_min[0]) - x, x - double(m.box_max[0])});
    const double dz = std::max({0.0, double(m.box_min[2]) - z, z - double(m.box_max[2])});
    if (dx * dx + dz * dz > r * r) continue;
    near_ids.push_back(i);
  }
  std::vector<std::pair<uint32_t, fh1::RenderModel>> selected;
  // The materials always: the effects tell the far ring's objects apart.
  if (!load_models_parallel(near_ids, true, selected)) return false;
  WorldLodFilter lowest;
  for (const auto& [i, model] : selected)
    for (const auto& o : model.objects) {
      if (skipped_object(o.name) || far_level(o.name, object_effects(model, o)) != 0) continue;
      int lod;
      const std::string family = lod_family(o.name, lod);
      if (lod < 0) continue;
      std::array<float, 3> bmin, bmax;
      object_bounds(o, model, bmin, bmax);
      lowest.add(family, lod, bmin, bmax);
    }
  for (const auto& [i, model] : selected) add_triangles(model, i, nullptr, &lowest);
  st.world_models = selected.size();

  // The far ring: MIDDIST and UberLOD objects of the models within
  // far_radius, the finest LOD present per co-located family.
  if (options.render_data && options.far_radius > 0) {
    std::vector<uint32_t> far_ids;
    const double fr = options.far_radius;
    for (uint32_t i = 0; i < models_.size(); ++i) {
      const ModelInfo& m = models_[i];
      if (!m.ok || !m.world_space || !m.far) continue;
      const double dx = std::max({0.0, double(m.box_min[0]) - x, x - double(m.box_max[0])});
      const double dz = std::max({0.0, double(m.box_min[2]) - z, z - double(m.box_max[2])});
      if (dx * dx + dz * dz > fr * fr) continue;
      far_ids.push_back(i);
    }
    std::vector<std::pair<uint32_t, fh1::RenderModel>> far_models;
    if (!load_models_parallel(far_ids, true, far_models)) return false;
    WorldLodFilter far_lowest;
    for (const auto& [i, model] : far_models)
      for (const auto& o : model.objects) {
        int lod;
        const std::string family = lod_family(o.name, lod);
        if (lod < 0 || far_level(o.name, object_effects(model, o)) == 0) continue;
        std::array<float, 3> bmin, bmax;
        object_bounds(o, model, bmin, bmax);
        far_lowest.add(family, lod, bmin, bmax);
      }
    // The hole filling: the near ground's footprint within the widest
    // inner cut (game frame; z is the file frame's here), then each hole
    // cell's finest far object (MIDDIST preferred over UberLOD, and 4 m
    // height-aware cells so steep MIDDIST hillsides rising above a lower
    // roadside patch are not cut out).
    cover = std::make_unique<GroundCover>(x, -z, std::max(options.far_inner, options.far_inner_uberlod), 4.0);
    for (size_t t = 0; t < out.triangles(); ++t)
      if (out.surface[t] != Surface::kWater)
        cover->add(out.positions[3 * t], out.positions[3 * t + 1], out.positions[3 * t + 2]);
    // A submesh the far pass draws nothing of: the placeholder texture
    // (WORLD-MATERIALS, Placeholder models) or the UNDERGROUND effect.
    auto drawn = [&](uint32_t model_index, const fh1::RenderModel& model, const fh1::RmbSubmesh& s) {
      if (skipped_material(s.material)) return false;
      if (s.material_index >= model.materials.size()) return true;
      const fh1::RmbMaterial& m = model.materials[s.material_index];
      if (m.effect < model.effects.size() && model.effects[m.effect] == "UNDERGROUND") return false;
      const std::string& name = models_[model_index].name;
      const size_t dot = name.find('.');
      const unsigned long member = dot != std::string::npos ? std::strtoul(name.c_str() + dot + 1, nullptr, 10) : 0;
      if (m.slots.empty() || m.slots[0] < 0 || member < 1 || member - 1 >= impl_->table.model_textures.size())
        return true;
      const auto& list = impl_->table.model_textures[member - 1];
      if (size_t(m.slots[0]) >= list.size() || list[size_t(m.slots[0])] >= impl_->table.textures.size()) return true;
      return impl_->table.textures[list[size_t(m.slots[0])]].id != kPlaceholderTextureId;
    };
    for (const auto& [i, model] : far_models)
      for (const auto& o : model.objects) {
        const uint8_t level = far_level(o.name, object_effects(model, o));
        if (level == 0) continue;
        int lod;
        const std::string family = lod_family(o.name, lod);
        if (lod >= 0) {
          std::array<float, 3> bmin, bmax;
          object_bounds(o, model, bmin, bmax);
          if (far_lowest.suppressed(family, lod, bmin, bmax)) continue;
        }
        const double inner = level == 2 ? options.far_inner_uberlod : options.far_inner;
        double area = 0;
        size_t count = 0;
        std::vector<long> cells;
        for (const auto& s : o.submeshes) {
          if (!drawn(i, model, s)) continue;
          const auto tri = fh1::rmb_triangle_list(s);
          for (size_t t = 0; t + 2 < tri.size(); t += 3) {
            std::array<std::array<float, 3>, 3> p;
            bool inside = true;
            for (int k = 0; k < 3; ++k) {
              const auto q = o.position(tri[t + size_t(k)]);
              p[size_t(k)] = {q[0], q[1], -q[2]};
              inside = inside && std::hypot(p[size_t(k)][0] - x, p[size_t(k)][2] + z) < inner;
            }
            area += std::fabs((double(p[1][0]) - p[0][0]) * (double(p[2][2]) - p[0][2]) -
                              (double(p[2][0]) - p[0][0]) * (double(p[1][2]) - p[0][2])) / 2;
            ++count;
            if (!inside) continue;
            for (const auto& sp : footprint_samples_3d(p[0], p[1], p[2]))
              if (!cover->covered_at(sp[0], sp[1], sp[2], 1.5)) cells.push_back(cover->cell_of(sp[0], sp[2]));
          }
        }
        if (cells.empty()) continue;
        const std::tuple<uint8_t, double, uint64_t> filler{level, area / double(count), object_key(i, model, o)};
        for (long c : cells) {
          auto [it, added] = hole_fill.emplace(c, filler);
          if (!added && filler < it->second) it->second = filler;
        }
      }
    for (const auto& [i, model] : far_models) add_triangles(model, i, nullptr, &far_lowest, true);
    cover.reset();
    st.far_models = far_models.size();
  }

  // Tree sprites near the point (WORLD-MATERIALS, Tree billboards): the
  // file records only; the generated records of placed instances are not
  // made (ours, for now). Two triangles per sprite, corners 0 1 2 and 0 2 3.
  if (options.render_data && options.tree_radius > 0 && impl_->have_props) {
    std::map<uint32_t, uint32_t> material_of;  // atlas record -> material
    for (const auto& t : impl_->props.trees) {
      const double dx = t.base[0] - x, dz = t.base[2] + z, d = std::sqrt(dx * dx + dz * dz);  // z: file frame here
      if (d > options.tree_radius || d > t.far) continue;
      if (options.route >= 0 && !fh1::shown_on_route(t.routes, options.route)) {
        ++st.trees_off_route;
        continue;
      }
      auto mit = material_of.find(t.atlas);
      if (mit == material_of.end()) {
        MeshMaterial mm;
        mm.effect = "TREE_BILLBOARD";  // ours: the sprite shader's name is not in the files
        mm.blend = Blend::kCoverage;
        mm.layers = Layers::kBillboard;
        if (t.atlas < impl_->table.textures.size()) mm.albedo = t.atlas;  // a file or a bundle entry
        auto [it, fresh] = material_index.emplace(mm.key(), uint32_t(out.materials.size()));
        if (fresh) out.materials.push_back(mm);
        mit = material_of.emplace(t.atlas, it->second).first;
      }
      if (out.materials[mit->second].albedo == kNoTexture) {
        ++st.trees_untextured;
        continue;
      }
      double len = std::sqrt(double(t.axis[0]) * t.axis[0] + double(t.axis[1]) * t.axis[1] + double(t.axis[2]) * t.axis[2]);
      const std::array<float, 3> up = len > 0.5 ? std::array<float, 3>{float(t.axis[0] / len), float(t.axis[1] / len),
                                                                       float(t.axis[2] / len)}
                                                : std::array<float, 3>{0, 1, 0};
      const std::array<std::array<float, 2>, 4> offset{{{-t.half_width, 0}, {t.half_width, 0},
                                                        {t.half_width, t.height}, {-t.half_width, t.height}}};
      const std::array<std::array<float, 2>, 4> uv{{{t.u0, t.v1}, {t.u1, t.v1}, {t.u1, t.v0}, {t.u0, t.v0}}};
      for (int c : {0, 1, 2, 0, 2, 3}) {
        out.positions.push_back(t.base);
        out.normals.push_back(up);
        out.uv0.push_back(uv[size_t(c)]);
        out.uv1.push_back(offset[size_t(c)]);
        out.uv2.push_back({0.0f, 0.0f});
        out.colours.push_back(t.tint);
      }
      for (int k = 0; k < 2; ++k) {
        out.surface.push_back(Surface::kProp);
        out.model.push_back(0);
        out.ground.push_back(0);
        out.material.push_back(mit->second);
        out.instance.push_back(t.base);
      }
      ++st.trees;
    }
  }

  // Props near the point, one per model and spot (a zone lists one object
  // per LOD level at the same spot). Placed 3D trees beyond prop_radius and
  // within tree_radius receive runtime camera-facing distant billboards.
  if (options.props && impl_->have_props) {
    std::set<std::tuple<int64_t, long long, long long, long long>> once;
    std::map<std::string, uint32_t> index_of;
    for (uint32_t i = 0; i < models_.size(); ++i) index_of[models_[i].name] = i;
    const double prop_r2 = options.prop_radius * options.prop_radius;
    const double tree_r2 = (options.render_data && options.tree_radius > options.prop_radius)
                               ? (options.tree_radius * options.tree_radius)
                               : prop_r2;
    // Pre-decode all unique prop models (and their immediate LOD neighbors) in parallel
    // before the sequential triangle assembly loop so PropModels::model() hits cache.
    {
      std::vector<uint32_t> preload_members;
      for (const auto& p : impl_->props.placements) {
        if (!p.has_object || p.object_id >= impl_->table.objects.size()) continue;
        const auto& t = p.placement.offset;
        // Skip unplaced origin-sentinel records in Colorado_00.pvs (offset == {0,0,0})
        // that otherwise dump ~9,700 props / 11M triangles whenever within prop_radius of (0,0,0).
        if (std::fabs(t[0]) < 1e-3 && std::fabs(t[1]) < 1e-3 && std::fabs(t[2]) < 1e-3) continue;
        const double dx = t[0] - x, dz = t[2] - z;
        if (dx * dx + dz * dz > tree_r2) continue;
        if (options.route >= 0 && !fh1::shown_on_route(p.routes, options.route)) continue;
        const uint32_t m = impl_->table.objects[p.object_id].member;
        preload_members.push_back(m);
        if (m > 0) preload_members.push_back(m - 1);
        preload_members.push_back(m + 1);
        preload_members.push_back(m + 2);
      }
      impl_->prop_models->preload(preload_members);
    }
    std::map<uint32_t, uint32_t> distant_tree_mat;
    for (const auto& p : impl_->props.placements) {
      const auto& t = p.placement.offset;
      if (std::fabs(t[0]) < 1e-3 && std::fabs(t[1]) < 1e-3 && std::fabs(t[2]) < 1e-3) continue;
      const double dx = t[0] - x, dz = t[2] - z, d2 = dx * dx + dz * dz;
      if (d2 > tree_r2) continue;
      const bool distant_tree_band = d2 > prop_r2;
      if (options.route >= 0 && !fh1::shown_on_route(p.routes, options.route)) {
        ++st.props_off_route;
        continue;
      }
      if (options.hidden && !p.name.empty() && options.hidden->count(p.name)) {
        ++st.props_hidden;
        continue;
      }
      if (options.hidden_positions && !options.hidden_positions->empty()) {
        const float gx = float(t[0]), gy = float(t[1]), gz = float(-t[2]);
        bool smashed = false;
        for (const auto& hs : *options.hidden_positions) {
          if (hs.radius <= 0.0f) continue;
          // Normal smashables snap to the exact .pvsz placement origin (< 0.05 m), so clamp
          // their gather match radius tightly to avoid hiding adjacent intact props when
          // hs.radius is large; persistent discount signs keep a wider trigger tolerance.
          const float hr = hs.persistent ? std::min(hs.radius, 3.5f)
                                         : std::min(hs.radius, std::clamp(hs.radius * 0.40f, 0.55f, 1.15f));
          const float hdx = gx - hs.pos[0], hdy = gy - hs.pos[1], hdz = gz - hs.pos[2];
          if (hdx * hdx + hdz * hdz <= hr * hr && std::fabs(hdy) <= hr + 1.5f) {
            smashed = true;
            break;
          }
        }
        if (smashed) {
          ++st.props_hidden;
          continue;
        }
      }
      if (!p.has_object || p.object_id >= impl_->table.objects.size()) {
        ++st.prop_unresolved;
        continue;
      }
      const uint32_t m = impl_->table.objects[p.object_id].member;
      // The placeholder marker of activity nodes is never drawn (WORLD-
      // GEOMETRY: a guess from the one marker in view).
      std::string prop_family_low;
      {
        std::string family;
        int lod = -1;
        if (impl_->prop_models->first_name(m, family, lod)) {
          for (char& ch : family) ch = char(std::tolower(static_cast<unsigned char>(ch)));
          prop_family_low = family;
          if (family.rfind("temp_arrow_pointer", 0) == 0) {
            ++st.props_markers;
            continue;
          }
        }
      }
      if (distant_tree_band) {
        const bool is_tree = prop_family_low.find("tree") != std::string::npos ||
                             prop_family_low.find("pine") != std::string::npos ||
                             prop_family_low.find("aspen") != std::string::npos ||
                             prop_family_low.find("spruce") != std::string::npos ||
                             prop_family_low.find("birch") != std::string::npos ||
                             prop_family_low.find("cedar") != std::string::npos ||
                             prop_family_low.find("fir_") != std::string::npos;
        if (!is_tree) continue;
        const int64_t far_member = impl_->prop_models->lod_member(m, 2);
        if (far_member < 0) continue;
        if (!once.insert({far_member, std::llround(t[0] * 100), std::llround(t[1] * 100), std::llround(t[2] * 100)})
                 .second) {
          continue;
        }
        const fh1::RenderModel* tm = impl_->prop_models->model(uint32_t(far_member));
        if (!tm) continue;
        const std::string& mname = impl_->prop_models->name_of(uint32_t(far_member));
        const size_t dot = mname.find('.');
        const unsigned long member_id =
            dot != std::string::npos ? std::strtoul(mname.c_str() + dot + 1, nullptr, 10) : 0;
        uint32_t tex_rec = kNoTexture;
        if (member_id >= 1 && member_id - 1 < impl_->table.model_textures.size()) {
          for (uint32_t rec : impl_->table.model_textures[member_id - 1]) {
            if (rec < impl_->table.textures.size()) {
              const auto& tr = impl_->table.textures[rec];
              if (tr.id != kPlaceholderTextureId &&
                  (tr.type == fh1::kPvsTextureBix || tr.type == fh1::kPvsTextureCaff)) {
                tex_rec = rec;
                break;
              }
            }
          }
        }
        if (tex_rec == kNoTexture) continue;
        auto mit = distant_tree_mat.find(tex_rec);
        if (mit == distant_tree_mat.end()) {
          MeshMaterial mm;
          mm.effect = "TREE_BILLBOARD";
          mm.blend = Blend::kCoverage;
          mm.layers = Layers::kBillboard;
          mm.albedo = tex_rec;
          auto [it, fresh] = material_index.emplace(mm.key(), uint32_t(out.materials.size()));
          if (fresh) out.materials.push_back(mm);
          mit = distant_tree_mat.emplace(tex_rec, it->second).first;
        }
        const float half_w = std::clamp(0.5f * std::max(tm->box_max[0] - tm->box_min[0],
                                                        tm->box_max[2] - tm->box_min[2]),
                                        1.5f, 12.0f);
        const float height = std::clamp(tm->box_max[1] - tm->box_min[1], 3.0f, 26.0f);
        const auto gp = p.placement.apply({0.0f, tm->box_min[1], 0.0f});
        const std::array<float, 3> base{float(gp[0]), float(gp[1]), float(-gp[2])};
        const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
        const std::array<std::array<float, 2>, 4> offset{{{-half_w, 0.0f}, {half_w, 0.0f},
                                                          {half_w, height}, {-half_w, height}}};
        const std::array<std::array<float, 2>, 4> uv{{{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}};
        const std::array<uint8_t, 4> tint{128, 128, 128, 255};
        for (int c : {0, 1, 2, 0, 2, 3}) {
          out.positions.push_back(base);
          out.normals.push_back(up);
          out.uv0.push_back(uv[size_t(c)]);
          out.uv1.push_back(offset[size_t(c)]);
          out.uv2.push_back({0.0f, 0.0f});
          out.colours.push_back(tint);
        }
        for (int k = 0; k < 2; ++k) {
          out.surface.push_back(Surface::kProp);
          out.model.push_back(0);
          out.ground.push_back(0);
          out.material.push_back(mit->second);
          out.instance.push_back(base);
        }
        ++st.trees;
        continue;
      }
      const bool fading = options.render_data && options.prop_near_radius > 0;
      const bool resolvable = impl_->prop_models->model(m) != nullptr;
      if (!resolvable) {
        const bool no_tex_list =
            (m < 1 || m - 1 >= impl_->table.model_textures.size() || impl_->table.model_textures[m - 1].empty());
        if (no_tex_list && impl_->source.contains(std::to_string(m) + ".fiz")) {
          ++st.props_physics_only;
          continue;
        }
      }
      // The levels drawn here, with their fade classes.
      // Large buildings/landmarks (>= 25 m extent) use fade = 0 so they never
      // split across 64 m LOD cells, drop sub-objects on stripped LOD02 at
      // 80-120 m, or fade out at 310-350 m beside 900 m world-space wings.
      // Smaller 3+ LOD props fall back from LOD02 to LOD01 when LOD02 loses
      // sub-objects or shrinks significantly compared to LOD00.
      struct Level {
        int64_t member = -1;
        uint8_t fade = 0;
      } levels[2];
      if (resolvable) {
        const int64_t near_member = impl_->prop_models->lod_member(m, 0);
        const int64_t mid_member = impl_->prop_models->lod_member(m, 1);
        int64_t far_member = impl_->prop_models->lod_member(m, 2);
        const fh1::RenderModel* r0 = near_member >= 0 ? impl_->prop_models->model(uint32_t(near_member)) : nullptr;
        const float max_extent = r0 ? box_max_extent(r0->box_min, r0->box_max) : 0.0f;
        const bool has_distinct_mid =
            mid_member >= 0 && mid_member != near_member && mid_member != far_member;
        if (r0 && has_distinct_mid && far_member >= 0) {
          const fh1::RenderModel* r2 = impl_->prop_models->model(uint32_t(far_member));
          if (r2 && prop_lod_degraded(r0->box_min, r0->box_max, r0->objects.size(),
                                      r2->box_min, r2->box_max, r2->objects.size())) {
            far_member = mid_member;
          }
        }
        const bool large_structure = max_extent >= kLargeBuildingExtent;
        if (fading) {
          if (large_structure) {
            int64_t chosen = near_member;
            if (d2 >= options.prop_near_radius * options.prop_near_radius && has_distinct_mid) {
              const fh1::RenderModel* r1 = impl_->prop_models->model(uint32_t(mid_member));
              if (r0 && r1 && !prop_lod_degraded(r0->box_min, r0->box_max, r0->objects.size(),
                                                 r1->box_min, r1->box_max, r1->objects.size())) {
                chosen = mid_member;
              }
            }
            levels[0] = {chosen, uint8_t(0)};
          } else if (near_member == far_member) {
            levels[0] = {far_member, uint8_t(3)};
          } else {
            levels[0] = {far_member, uint8_t(2)};
            if (d2 < options.prop_near_radius * options.prop_near_radius) levels[1] = {near_member, uint8_t(1)};
          }
        } else {
          if (large_structure) {
            levels[0] = {near_member, uint8_t(0)};
          } else {
            levels[0] = {d2 < options.prop_lod0 * options.prop_lod0 ? near_member : far_member, uint8_t(0)};
          }
        }
      }
      // The gameplay beacon column (WORLD-MATERIALS item 7: coloradoout.06509,
      // the only DIFF_GLOW_1 model, 105 placements) never drew in the frames
      // checked, day or night: interim rule, not drawn.
      if (impl_->prop_models->name_of(m) == "coloradoout.06509.rmb.bin") {
        ++st.props_beacons;
        continue;
      }
      bool drawn = false, beacon = false;
      for (const auto& [member, fade] : levels) {
        const fh1::RenderModel* model = member >= 0 ? impl_->prop_models->model(uint32_t(member)) : nullptr;
        if (!model) continue;
        if (!once.insert({member, std::llround(t[0] * 100), std::llround(t[1] * 100), std::llround(t[2] * 100)})
                 .second) {
          drawn = true;  // the same spot's other LOD object
          continue;
        }
        if (impl_->prop_models->name_of(uint32_t(member)) == "coloradoout.06509.rmb.bin") {
          beacon = true;
          continue;
        }
        const auto it = index_of.find(impl_->prop_models->name_of(uint32_t(member)));
        add_triangles(*model, it != index_of.end() ? it->second : 0, &p.placement, nullptr, false, fade);
        // Emit night streetlamp / floodlight fixture glow sprites once per prop instance.
        // Match only actual light fixture props (never water towers, brick walls, signs,
        // tents, gantries, or booths).
        if (options.render_data && fade != 1 && !prop_family_low.empty()) {
          const bool is_streetlamp = prop_family_low.find("lamppost") != std::string::npos ||
                                     prop_family_low.find("streetlight") != std::string::npos ||
                                     prop_family_low.find("carpark_lights") != std::string::npos ||
                                     prop_family_low.find("lightcasings") != std::string::npos;
          const bool is_floodlight = prop_family_low.find("floodlight") != std::string::npos ||
                                     prop_family_low.find("spotlight") != std::string::npos ||
                                     prop_family_low.rfind("lights_lod", 0) == 0;
          if (is_streetlamp || is_floodlight) {
            const float local_top = std::max(model->box_max[1] * 0.94f, 0.35f);
            const auto gp = p.placement.apply({0.0f, local_top, 0.0f});
            WorldGlow wg;
            wg.position = {float(gp[0]), float(gp[1]), float(-gp[2])};
            if (is_floodlight) {
              // Outpost / festival stadium floodlight tower: crisp warm-white metal-halide glow.
              wg.color = {1.0f, 0.96f, 0.88f};
              wg.radius = (model->box_max[1] > 10.0f) ? 1.85f : 0.85f;
            } else {
              // Streetlamp / lamppost warm sodium-amber glow.
              wg.color = {1.0f, 0.78f, 0.42f};
              wg.radius = 1.25f;
            }
            // Deterministic per-instance threshold in [0.05, 0.90] for staggered dusk switch-on.
            const uint32_t h = uint32_t(std::llround(t[0] * 7.0)) * 2654435761u ^
                               uint32_t(std::llround(t[2] * 7.0)) * 2246822519u;
            wg.threshold = 0.05f + 0.85f * float(h & 0xFFFFu) / 65535.0f;
            out.world_glows.push_back(wg);
          }
        }
        drawn = true;
        if (fade != 1) ++st.props;
        else ++st.props_near_lod;
      }
      if (beacon) ++st.props_beacons;
      else if (!drawn) ++st.prop_unresolved;
    }
  }
  return true;
}

bool find_spawn(const Mesh& mesh, double x, double z, Spawn& out, const double* prefer_heading) {
  double best = -1;
  for (size_t t = 0; t < mesh.triangles(); ++t) {
    if (mesh.surface[t] != Surface::kRoad) continue;
    const auto& a = mesh.positions[t * 3];
    const auto& b = mesh.positions[t * 3 + 1];
    const auto& c = mesh.positions[t * 3 + 2];
    const double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
    const double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 0) || std::fabs(n[1]) / len < 0.95) continue;  // flat enough to stand on
    if (len / 2 < 2.0) continue;                                // at least 2 m^2, a car's footprint shares it
    const double cx = (double(a[0]) + b[0] + c[0]) / 3, cz = (double(a[2]) + b[2] + c[2]) / 3;
    const double d = std::hypot(cx - x, cz - z);
    if (best >= 0 && d >= best) continue;
    best = d;
    out.position = {cx, (double(a[1]) + b[1] + c[1]) / 3, cz};
    out.distance = d;
    // The longest edge.
    const std::array<float, 3>* corners[3] = {&a, &b, &c};
    double longest = -1, hx = 0, hz = 1;
    for (int k = 0; k < 3; ++k) {
      const auto& p = *corners[k];
      const auto& q = *corners[(k + 1) % 3];
      const double ex = double(q[0]) - p[0], ez = double(q[2]) - p[2], l = std::hypot(ex, ez);
      if (l > longest) {
        longest = l;
        hx = ex;
        hz = ez;
      }
    }
    const bool flip = prefer_heading ? hx * std::sin(*prefer_heading) + hz * std::cos(*prefer_heading) < 0
                                     : (hz < 0 || (hz == 0 && hx < 0));
    if (flip) {
      hx = -hx;
      hz = -hz;
    }
    out.heading = std::atan2(hx, hz);
  }
  return best >= 0;
}

}  // namespace world
