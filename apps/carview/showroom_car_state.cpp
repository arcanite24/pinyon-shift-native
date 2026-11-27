#include "showroom_internal.h"

#include "cars/bake.h"
#include "cars/texture_mods.h"
#include "cars/visual_setup.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>

namespace carview {

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
    const int32_t min_lod = options_.prefer_lod1 ? std::max(options_.prefer_min_lod, 1) : 0;
    const int32_t best = best_lod(part, min_lod);
    if (best < 0) continue;
    const int32_t far_lod = options_.prefer_lod1 ? far_lod_for(part, best) : -1;
    const auto& pool = best == 0 ? part.lod0_vertices : part.lod_vertices;
    const int32_t vertex_offset = int32_t(vertices_.size());
    vertices_.insert(vertices_.end(), pool.begin(), pool.end());
    const size_t draws_before = draws_.size();
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
      const bool is_far_sub = (far_lod > best && s.lod == far_lod);
      if ((s.lod != best && !is_far_sub) || s.indices.empty()) continue;
      const cars::BlurRimKind blur_kind = cars::classify_blur_rim(s.technique);
      if (s.material == MaterialClass::kHidden && blur_kind == cars::BlurRimKind::kNone) continue;
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
      std::array<float, 3> pmin{1e9f, 1e9f, 1e9f}, pmax{-1e9f, -1e9f, -1e9f};
      for (uint32_t i : s.indices) {
        if (i >= pool.size()) {
          bad = true;
          break;
        }
        if (model.role != Role::kWheel && model.role != Role::kTyre &&
            pool[i].lighting[cars::kCrushWeightLightingChannel] > 0) {
          car_has_crush_weights_ = true;
        }
        const auto p = transform_point(transform, pool[i].position);
        if (!have_bounds_) {
          bounds_min_ = bounds_max_ = p;
          have_bounds_ = true;
        }
        for (int k = 0; k < 3; ++k) {
          bounds_min_[size_t(k)] = std::min(bounds_min_[size_t(k)], p[size_t(k)]);
          bounds_max_[size_t(k)] = std::max(bounds_max_[size_t(k)], p[size_t(k)]);
          pmin[size_t(k)] = std::min(pmin[size_t(k)], p[size_t(k)]);
          pmax[size_t(k)] = std::max(pmax[size_t(k)], p[size_t(k)]);
          sum[size_t(k)] += p[size_t(k)];
        }
      }
      if (bad) {
        notes_.push_back(model.name + "/" + part.name + "/" + s.technique + ": index out of range");
        continue;
      }
      for (int k = 0; k < 3; ++k) d.centre[size_t(k)] = sum[size_t(k)] / float(s.indices.size());
      d.part_min = pmin;
      d.part_max = pmax;
      if (std::getenv("CARVIEW_SECTIONS")) {
        std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (uint32_t i : s.indices) {
          const auto q = transform_point(transform, pool[i].position);
          for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
        }
        std::printf("drawn %s/%s %s: x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", model.name.c_str(), part.name.c_str(),
                    s.technique.c_str(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
      }
      // When `transform` has a negative 3x3 determinant (e.g. body_to_model_ mirrors Z so
      // det = -1 on all wheel/tyre/upgraded-brake draws), triangle winding is inverted in
      // clip space and VK_CULL_MODE_BACK_BIT culls the outer faces of every tyre and rim.
      // Flip triangle winding (i0, i2, i1) so outward faces stay front-facing under culling.
      const float det3 = transform[0] * (transform[5] * transform[10] - transform[6] * transform[9]) -
                         transform[4] * (transform[1] * transform[10] - transform[2] * transform[9]) +
                         transform[8] * (transform[1] * transform[6] - transform[2] * transform[5]);
      if (det3 < 0.0f) {
        const size_t n = s.indices.size();
        indices_.reserve(indices_.size() + n);
        for (size_t k = 0; k + 2 < n; k += 3) {
          indices_.push_back(s.indices[k]);
          indices_.push_back(s.indices[k + 2]);
          indices_.push_back(s.indices[k + 1]);
        }
        for (size_t k = (n / 3) * 3; k < n; ++k) indices_.push_back(s.indices[k]);
      } else {
        indices_.insert(indices_.end(), s.indices.begin(), s.indices.end());
      }
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
      // Tire models (TireC / per-car TireA*) can carry the transparent subsection flag in
      // .shiftbake packs, which causes cars::classify() to return kGlass before checking kTyre.
      // Force tyre techniques and Role::kTyre meshes to stay opaque rubber (kTyre).
      if (model.role == Role::kTyre || technique == "tire" || technique == "sidewall" || technique == "tread") {
        klass = MaterialClass::kTyre;
      }
      if (klass == MaterialClass::kOpaque && d.lamp_channel != drive::LampChannel::kNone) {
        klass = (d.lamp_channel == drive::LampChannel::kGaugeBacklight ||
                 d.lamp_channel == drive::LampChannel::kDigitalGauge ||
                 d.lamp_channel == drive::LampChannel::kNumPlate)
                    ? MaterialClass::kGauge
                    : MaterialClass::kLamp;
      }
      const bool is_defrost = (technique == "defrost_lines");
      if (blur_kind != cars::BlurRimKind::kNone) {
        d.is_blur_rim = true;
        flags |= kFlagBlurRim;
      }
      // Resolve slot 1 (with fallback for existing v3 .shiftbake packs whose slots[1] was empty).
      std::string slot1 = s.slots[1];
      if (slot1.empty()) {
        const std::string pname_l = lower(part.name);
        if (d.is_blur_rim) {
          slot1 = "wheel:wheel0.xds";
        } else if (klass == MaterialClass::kGauge && technique == "gauge_emissive") {
          slot1 = "car:interior_emissive_lod0.xds";
        } else if (klass == MaterialClass::kInterior || klass == MaterialClass::kGauge) {
          slot1 = "car:interior_lod0.xds";
        } else if (klass == MaterialClass::kLamp ||
                   (klass == MaterialClass::kGlass && technique.rfind("detail_glass_", 0) == 0)) {
          slot1 = (s.lod == 0) ? "car:lights_lod0.xds" : "car:lights.xds";
        } else if (klass == MaterialClass::kCarbon) {
          slot1 = "shared:carbonfiber.xds";
        } else if (klass == MaterialClass::kOpaque &&
                   (technique == "undercarriage" || technique == "underbody" ||
                    pname_l.find("undercarriage") != std::string::npos ||
                    pname_l.find("underbody") != std::string::npos)) {
          slot1 = "shared:undercarriage.xds";
        }
      } else if (klass == MaterialClass::kGauge && technique == "gauge_emissive") {
        // Prefer real backlit dial texture when available on gauge_emissive cluster faces.
        if (texture_index("car:interior_emissive_lod0.xds", pack, true, /*optional=*/true) != UINT32_MAX) {
          slot1 = "car:interior_emissive_lod0.xds";
        }
      }
      // Pre-register wheel0..3.xds speed-tier blur textures when present in the wheel pack.
      if (d.is_blur_rim) {
        for (const char* wtex : {"wheel:wheel0.xds", "wheel:wheel1.xds", "wheel:wheel2.xds", "wheel:wheel3.xds"}) {
          (void)texture_index(wtex, pack, s.lod == 0, /*optional=*/true);
        }
      }
      // Glass techniques never sample the body atlas (FORMAT-CARS, Paint):
      // an opaque glass draw (the windscreen's black_glass band) is a flat
      // dark grey; defrost_lines is rendered as thin translucent copper-amber heater lines.
      const bool flat_glass = (klass != MaterialClass::kGlass && s.slots[0] == "@atlas" &&
                               technique.find("glass") != std::string::npos) ||
                              is_defrost;
      d.uses_atlas = (!flat_glass && s.slots[0] == "@atlas");
      d.atlas_lod0 = (s.lod == 0);
      const uint32_t t0 = flat_glass ? UINT32_MAX : texture_index(s.slots[0], pack, s.lod == 0);
      const bool t1_has_fallback = (slot1 == "car:interior_emissive_lod0.xds" || slot1 == "car:lights.xds");
      uint32_t t1 = texture_index(slot1, pack, s.lod == 0, /*optional=*/t1_has_fallback);
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
        // FORMAT-CARS: Data_Car.CaliperRGBFront/Rear are passed directly as linear 0..1 floats
        // (without sRGB linearization, which crushes red/gold Brembo calipers too dark).
        const uint32_t rgb = info.caliper_rgb[size_t(axle)];
        d.push.tint[0] = float((rgb >> 16) & 0xFF) / 255.0f;
        d.push.tint[1] = float((rgb >> 8) & 0xFF) / 255.0f;
        d.push.tint[2] = float(rgb & 0xFF) / 255.0f;
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
      // Probe optional per-car overrides silently before falling back to Shared.zip so
      // shared interior normal maps (plastic/plastic2/leather/stitches) emit zero false
      // missing-texture notes when resolved via media/cars/Shared.zip.
      auto try_tex = [&](std::initializer_list<const char*> candidates, bool warn_if_missing = false) -> uint32_t {
        for (const char* c : candidates) {
          const uint32_t idx = texture_index(c, pack, s.lod == 0, /*optional=*/true);
          if (idx != UINT32_MAX) return idx;
        }
        if (warn_if_missing && candidates.size() > 0) {
          (void)texture_index(*candidates.begin(), pack, s.lod == 0, /*optional=*/false);
        }
        return UINT32_MAX;
      };
      uint32_t t_nrm = UINT32_MAX;
      bool nrm_uv1 = false;
      if (klass == MaterialClass::kCarbon) {
        t_nrm = try_tex({"shared:carbonfibernormal.xds"}, true);
        nrm_uv1 = true;
      } else if (klass == MaterialClass::kTyre) {
        if (technique == "sidewall") t_nrm = try_tex({"tyre:sidewall_nrm"});
        else if (technique == "tread") t_nrm = try_tex({"tyre:tread_nrm"});
      } else if (klass == MaterialClass::kInterior) {
        nrm_uv1 = true;
        if (technique == "bump_leather2" || technique == "leather2") {
          t_nrm = try_tex({"car:leather2_nrm.xds", "shared:leather2_nrm.xds", "shared:leather_nrm.xds"}, true);
        } else if (technique == "bump_leather3" || technique == "leather3") {
          t_nrm = try_tex({"car:leather3_nrm.xds", "shared:leather3_nrm.xds", "shared:leather_nrm.xds"}, true);
        } else if (technique == "bump_leather" || technique == "leather") {
          t_nrm = try_tex({"car:leather_nrm.xds", "shared:leather_nrm.xds"}, true);
        } else if (technique == "bump_plastic2" || technique == "plastic2") {
          t_nrm = try_tex({"car:plastic2_nrm.xds", "shared:plastic2_nrm.xds", "shared:plastic_nrm.xds"}, true);
        } else if (technique == "bump_plastic3" || technique == "plastic3") {
          t_nrm = try_tex({"car:plastic3_nrm.xds", "shared:plastic3_nrm.xds", "shared:plastic_nrm.xds"}, true);
        } else if (technique == "bump_plastic" || technique == "plastic") {
          t_nrm = try_tex({"car:plastic_nrm.xds", "shared:plastic_nrm.xds"}, true);
        } else if (technique == "stitching" || technique == "bump_stitching") {
          t_nrm = try_tex({"car:stitches_nrm.xds", "shared:stitches_nrm.xds"}, true);
        } else if (technique == "cloth" || technique == "cloth2") {
          t_nrm = try_tex({"shared:cloth_nrm_s_lod0.xds"}, true);
        } else if (technique == "headliner" || technique == "headliner2") {
          t_nrm = try_tex({"shared:headliner_nrm_s_lod0.xds"}, true);
        } else if (technique == "mottled" || technique == "mottled2") {
          t_nrm = try_tex({"shared:mottled_nrm_s_lod0.xds"}, true);
        }
      }
      if (t_nrm != UINT32_MAX) {
        flags |= kFlagNormalMap;
        if (nrm_uv1) flags |= kFlagNormalUv1;
      }
      // On kPaint draws, bind Shared/carbonfiber.xds (slot1) + Shared/carbonfibernormal.xds (normal_map)
      // under kFlagCarbonTex so Paint Shop Carbon Fiber finish samples real retail weave textures
      // without affecting standard paint shading.
      if (klass == MaterialClass::kPaint && t1 == UINT32_MAX && t_nrm == UINT32_MAX) {
        const uint32_t cf_d = texture_index("shared:carbonfiber.xds", pack, s.lod == 0);
        if (cf_d != UINT32_MAX) {
          t1 = cf_d;
          t_nrm = texture_index("shared:carbonfibernormal.xds", pack, s.lod == 0);
          flags |= kFlagCarbonTex;
        }
      }
      uint32_t t_dmg = UINT32_MAX;
      if ((klass == MaterialClass::kPaint || klass == MaterialClass::kOpaque) && s.slots[0] == "@atlas" &&
          !flat_glass) {
        t_dmg = (s.lod == 0) ? try_tex({"car:damage_lod0.xds", "car:damage.xds"})
                             : try_tex({"car:damage.xds", "car:damage_lod0.xds"});
      } else if (klass == MaterialClass::kLamp) {
        t_dmg = (s.lod == 0) ? try_tex({"car:damagelights_lod0.xds", "car:damagelights.xds"})
                             : try_tex({"car:damagelights.xds", "car:damagelights_lod0.xds"});
      }
      if (t_dmg != UINT32_MAX) flags |= kFlagDamageMap;
      // Alpha-tested emissive gauge overlays (z_gauge_emissive_alpha, gauge_emissive_alpha):
      // discard transparent surround so no solid rectangle blocks the gauge dials.
      if (klass == MaterialClass::kGauge &&
          (ends_with(technique, "_alpha") || technique.find("alpha") != std::string::npos)) {
        flags |= kFlagCutout;
      }
      d.push.tint[3] = 1.0f;
      if (klass == MaterialClass::kPaint) {
        const cars::ShaderFinishParams* sp = nullptr;
        if (colour) {
          if (auto it = car_shader_overrides_.color_overrides.find(int(colour->sequence));
              it != car_shader_overrides_.color_overrides.end()) {
            sp = &it->second;
          } else if (auto it2 = car_shader_overrides_.color_overrides.find(int(options_.colour + 1));
                     it2 != car_shader_overrides_.color_overrides.end()) {
            sp = &it2->second;
          }
        }
        if (!sp && shader_settings_.loaded_from_xml) {
          sp = (flags & kFlagMetallic) ? &shader_settings_.metallic : &shader_settings_.normal;
        }
        if (sp) d.push.tint[3] = std::clamp(sp->paint_scale, 0.15f, 1.85f);
      } else if (klass == MaterialClass::kCaliper && car_shader_overrides_.brake.has_override) {
        d.push.tint[3] = std::clamp(car_shader_overrides_.brake.params.paint_scale, 0.15f, 1.85f);
      }
      // Sub-material variant ID encoded in params[2] for showroom.frag:
      // 1=outer_rim (chrome lip), 2=inner_rim/wheel_black (dark barrel),
      // 3=front rotor/brake/drum, 13=rear rotor/brake/drum,
      // 4=hub, 5=cockpit metal/metal2, 6=defrost_lines, 9=non-window glass (interior/lamp lenses),
      // 10=mirrorLeft, 11=mirrorMiddle, 12=mirrorRight, 15=cockpit gauge needle,
      // 16=cockpit digital MFD gauge, 17=backlit cluster dial, 18=dark underbody/trim,
      // 20=front windshield (laminated), 21=rear window (tempered),
      // 22=left side window (tempered), 23=right side window (tempered).
      float submat_id = 0.0f;
      if (is_defrost) {
        submat_id = 6.0f;
      } else if (technique.rfind("mirror", 0) == 0) {
        if (technique == "mirrorleft") submat_id = 10.0f;
        else if (technique == "mirrorright") submat_id = 12.0f;
        else submat_id = 11.0f;
      } else if (klass == MaterialClass::kPaint) {
        const std::string pname_l = lower(part.name);
        if (pname_l.rfind("hood", 0) == 0) submat_id = 1.0f;
        else if (pname_l.rfind("wing", 0) == 0) submat_id = 2.0f;
        else if (pname_l.find("mirror") != std::string::npos) submat_id = 3.0f;
      } else if (klass == MaterialClass::kGlass && (flags & kFlagCover) == 0u) {
        const std::string pname_l = lower(part.name);
        if (pname_l == "interior" || pname_l.rfind("headlight", 0) == 0 ||
            pname_l.rfind("taillight", 0) == 0 || pname_l == "glasslhl" || pname_l == "glassrhl" ||
            pname_l == "glassltl" || pname_l == "glassrtl" ||
            technique == "lights_glass" || technique == "light_glass" || technique == "red_glass") {
          submat_id = 9.0f;
        } else if (pname_l == "glassf" ||
                   (pname_l.rfind("glassf", 0) == 0 && pname_l != "glassfl" && pname_l != "glassfr") ||
                   pname_l.find("windshield") != std::string::npos ||
                   pname_l.find("windscreen") != std::string::npos || technique == "frontwindow") {
          submat_id = 20.0f;
        } else if (pname_l == "glassr" || pname_l.rfind("trunk", 0) == 0 ||
                   pname_l.rfind("hatch", 0) == 0 || pname_l == "rearwindow") {
          submat_id = 21.0f;
        } else if (pname_l == "glasslf" || pname_l == "glasslr" || pname_l.rfind("doorl", 0) == 0) {
          submat_id = 22.0f;
        } else if (pname_l == "glassrf" || pname_l == "glassrr" || pname_l.rfind("doorr", 0) == 0) {
          submat_id = 23.0f;
        }
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
        else if (technique == "rotor" || technique == "brake" || technique == "drum") {
          submat_id = (pending_wheel_ >= 2 || axle == 1) ? 13.0f : 3.0f;
        } else if (technique == "hub") submat_id = 4.0f;
      } else if (klass == MaterialClass::kInterior) {
        std::string base_name = technique;
        while (!base_name.empty() && std::isdigit(static_cast<unsigned char>(base_name.back()))) base_name.pop_back();
        if (base_name == "metal") submat_id = 5.0f;
        // FORMAT-CARS interior technique albedo scales (ShaderSettings defaults):
        // mottled 0.35, bump_plastic 0.30, metal 0.85, plastic/cloth/leather/solid 0.50.
        const float k = base_name == "mottled" ? 0.35f :
                        base_name == "bump_plastic" ? 0.30f :
                        base_name == "metal" ? 0.85f : 0.50f;
        d.push.tint[0] = d.push.tint[1] = d.push.tint[2] = k;
      } else if (klass == MaterialClass::kOpaque) {
        // Underbody / chassis / frame / black trim / rubber trim techniques sample @atlas AO
        // (often bright grey/white canvas) or have no slot0 texture; tag submat 18 so showroom.frag
        // shades them as dark matte underbody charcoal instead of bright white.
        const std::string pname_l = lower(part.name);
        if (technique == "undercarriage" || technique == "underbody" || technique == "chassis" ||
            technique == "frame" || technique == "black" || technique == "black_noocclude" ||
            technique == "black_no_env" || technique == "rubber_trim" || technique == "weatherstrip" ||
            pname_l.find("undercarriage") != std::string::npos || pname_l.find("underbody") != std::string::npos) {
          submat_id = 18.0f;
        }
      }
      d.push.params[0] = float(uint32_t(klass));
      d.push.params[1] = float(flags);
      d.push.params[2] = submat_id;
      d.textures = {t0 == UINT32_MAX ? 0u : t0, t1 == UINT32_MAX ? 0u : t1,
                    livery == UINT32_MAX ? 1u : livery, t_nrm, t_dmg};
      d.glass = (klass == MaterialClass::kGlass) || d.is_blur_rim;
      d.part_name = lower(part.name);
      std::memcpy(d.rest_model, d.push.model, sizeof d.rest_model);
      if (d.wheel < 0 && d.cockpit_role < 0) {
        if (d.part_name.rfind("bumperf", 0) == 0) {
          d.anim_part = 3;
        } else if (d.part_name.rfind("bumperr", 0) == 0) {
          d.anim_part = 4;
        } else if (d.part_name == "mirrorl" || d.part_name.rfind("mirrorl", 0) == 0) {
          d.anim_part = 5;
        } else if (d.part_name == "mirrorr" || d.part_name.rfind("mirrorr", 0) == 0) {
          d.anim_part = 6;
        } else if (d.part_name.rfind("hood", 0) == 0) {
          d.anim_part = 11;
        } else if (d.part_name.rfind("wing", 0) == 0) {
          d.anim_part = 12;
        }
      }
      if (is_far_sub) {
        if (klass != MaterialClass::kInterior && klass != MaterialClass::kGauge && klass != MaterialClass::kCaliper) {
          far_draws_.push_back(d);
        }
      } else {
        draws_.push_back(d);
        drawn = true;
      }
    }
    if (options_.prefer_lod1 && far_lod < 0) {
      for (size_t di = draws_before; di < draws_.size(); ++di) {
        const uint32_t kl = uint32_t(draws_[di].push.params[0] + 0.5f);
        if (kl != 4u && kl != 5u && kl != 8u) far_draws_.push_back(draws_[di]);
      }
    }
    if (drawn) drawn_parts_.push_back(model.name + "/" + part.name);
  }
  // Optional diagnostic damage preset for deterministic headless verification:
  // CARVIEW_DAMAGE="front,rear,left,right,glass_crack,scratch[,body_dirt]"
  if (!guest_) {
    if (const char* dmg_env = std::getenv("CARVIEW_DAMAGE"); dmg_env && *dmg_env) {
      float f = 0.0f, r = 0.0f, l = 0.0f, ri = 0.0f, gc = 0.0f, sc = 0.0f, bd = 0.0f;
      const int n = std::sscanf(dmg_env, "%f,%f,%f,%f,%f,%f,%f", &f, &r, &l, &ri, &gc, &sc, &bd);
      if (n >= 4) {
        damage_zones_[0] = std::clamp(f, 0.0f, 1.0f);
        damage_zones_[1] = std::clamp(r, 0.0f, 1.0f);
        damage_zones_[2] = std::clamp(l, 0.0f, 1.0f);
        damage_zones_[3] = std::clamp(ri, 0.0f, 1.0f);
        damage_dirt_[0] = std::clamp(n >= 7 ? bd : 0.0f, 0.0f, 1.0f);
        damage_dirt_[1] = 0.0f;
        damage_dirt_[2] = std::clamp(n >= 5 ? gc : std::max(f, r), 0.0f, 1.0f);
        damage_dirt_[3] = std::clamp(n >= 6 ? sc : std::max({f, r, l, ri}), 0.0f, 1.0f);
        for (int i = 0; i < 4; ++i) {
          frame_.damage_dirt[i] = damage_dirt_[i];
          frame_.damage_zones[i] = damage_zones_[i];
        }
      }
    }
  }
}

void Showroom::set_braking(float glow) {
  const float g = std::clamp(glow, 0.0f, 1.0f);
  if (!lamp_state_active_) {
    lamp_vectors_.dynamic_lights_amount[0] = g;
  }
}

void Showroom::set_rotor_heat(float heat) {
  const auto split = split_rotor_heat_bias(heat, 0.70f);
  set_rotor_heat(split[0], split[1]);
}

void Showroom::set_rotor_heat(float front_heat, float rear_heat) {
  rotor_glow_front_ = std::clamp(front_heat, 0.0f, 1.0f);
  rotor_glow_rear_ = std::clamp(rear_heat, 0.0f, 1.0f);
  rotor_glow_ = rotor_glow_front_;
  frame_.dyn_light_meta[1] = rotor_glow_front_;
  frame_.dyn_light_meta[2] = rotor_glow_rear_;
}

void Showroom::sync_paint_shader_uniforms() {
  paint_shader_a_[0] = active_paint_params_.fresnel_index;
  paint_shader_a_[1] = active_paint_params_.fresnel_scalar;
  paint_shader_a_[2] = active_paint_params_.specular_cube_scalar;
  paint_shader_a_[3] = active_paint_params_.spec_amount;
  paint_shader_b_[0] = active_paint_params_.spec_amount2;
  paint_shader_b_[1] = active_paint_params_.spec_power2;
  paint_shader_b_[2] = active_paint_params_.spec_amount3;
  paint_shader_b_[3] = active_paint_params_.spec_power3;
  paint_shader_c_[0] = active_paint_params_.metal_color[0];
  paint_shader_c_[1] = active_paint_params_.metal_color[1];
  paint_shader_c_[2] = active_paint_params_.metal_color[2];
  paint_shader_c_[3] = active_paint_params_.metal_power;
  paint_shader_d_[0] = active_paint_params_.metal_bias;
  paint_shader_d_[1] = active_paint_params_.metal_scale;
  paint_shader_d_[2] = active_paint_params_.spec_power;
  float gauge_em = shader_settings_.gauge_emissive_scale;
  if (car_shader_overrides_.has_gauge_emissive_override) {
    gauge_em = car_shader_overrides_.gauge_emissive_scale;
  } else if (std::fabs(lamp_colors_.gauge_emissive - 2.5f) > 1e-3f) {
    gauge_em = lamp_colors_.gauge_emissive;
  }
  paint_shader_d_[3] = std::max(0.0f, gauge_em);

  rim_shader_[0] = car_shader_overrides_.rim.fresnel_index;
  rim_shader_[1] = car_shader_overrides_.rim.fresnel_scalar;
  rim_shader_[2] = car_shader_overrides_.rim.cube_scale;
  rim_shader_[3] = car_shader_overrides_.rim.has_override ? 1.0f : 0.0f;
  rim_tint_dirt_[0] = car_shader_overrides_.rim.tint_color[0];
  rim_tint_dirt_[1] = car_shader_overrides_.rim.tint_color[1];
  rim_tint_dirt_[2] = car_shader_overrides_.rim.tint_color[2];
  const uint32_t dirt_crush_bits = (has_car_dirt_mask_ ? 1u : 0u) | (car_has_crush_weights_ ? 2u : 0u);
  rim_tint_dirt_[3] = float(dirt_crush_bits);

  for (int i = 0; i < 4; ++i) {
    frame_.paint_shader_a[i] = paint_shader_a_[i];
    frame_.paint_shader_b[i] = paint_shader_b_[i];
    frame_.paint_shader_c[i] = paint_shader_c_[i];
    frame_.paint_shader_d[i] = paint_shader_d_[i];
    frame_.rim_shader[i] = rim_shader_[i];
    frame_.rim_tint_dirt[i] = rim_tint_dirt_[i];
  }
}

void Showroom::apply_damage_part_transforms(float time_sec) {
  auto make_rot_x = [](const std::array<float, 3>& pivot, float angle, float dy, float out[16]) {
    const float c = std::cos(angle), s = std::sin(angle);
    identity(out);
    out[5] = c;
    out[6] = s;
    out[9] = -s;
    out[10] = c;
    const float py = pivot[1], pz = pivot[2];
    out[13] = py + dy - (c * py - s * pz);
    out[14] = pz - (s * py + c * pz);
  };
  for (Draw& d : draws_) {
    if (d.wheel >= 0 || d.cockpit_role >= 0) continue;
    const bool det = (d.anim_part == 3 && dmg_front_bumper_detached_) ||
                     (d.anim_part == 4 && dmg_rear_bumper_detached_) ||
                     (d.anim_part == 5 && dmg_left_mirror_detached_) ||
                     (d.anim_part == 6 && dmg_right_mirror_detached_) ||
                     ((d.anim_part == 1 || d.anim_part == 8 || d.anim_part == 12) && dmg_wing_detached_);
    d.detached = det;
    if (det) {
      std::memset(d.push.model, 0, sizeof d.push.model);
      d.push.model[15] = 1.0f;
      continue;
    }
    if (d.anim_part == 11) {
      // Popped hood ajar: tilt around rear windshield cowl hinge (+Z max) lifting front lip (+Y).
      if (dmg_hood_ajar_ > 0.005f) {
        const std::array<float, 3> hinge{d.centre[0], d.part_max[1], d.part_max[2]};
        const float flutter = time_sec > 0.0f ? (std::sin(time_sec * 24.0f) * 0.012f * dmg_hood_ajar_) : 0.0f;
        const float tilt = 0.20f * dmg_hood_ajar_ + flutter;
        float xform[16];
        make_rot_x(hinge, tilt, 0.012f * dmg_hood_ajar_, xform);
        multiply(xform, d.rest_model, d.push.model);
      } else {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      }
    } else if (d.anim_part == 12) {
      // Fixed rear wing (not detached): keep at rest pose.
      std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
    } else if (d.anim_part >= 3 && d.anim_part <= 6) {
      // Severely damaged loose bumpers / side mirrors droop slightly & rattle with chassis vibration.
      const float zone_dmg =
          (d.anim_part == 3) ? damage_zones_[0] :
          (d.anim_part == 4) ? damage_zones_[1] :
          (d.anim_part == 5) ? damage_zones_[2] : damage_zones_[3];
      const float loose = std::clamp((zone_dmg * damage_dirt_[3] - 0.68f) / 0.32f, 0.0f, 1.0f);
      if (loose > 0.01f) {
        const float rattle =
            time_sec > 0.0f ? (std::sin(time_sec * 33.0f + float(d.anim_part) * 1.7f) * 0.014f * loose) : 0.0f;
        const float droop = -0.055f * loose;
        const float tilt = (d.anim_part == 3 ? 0.09f : (d.anim_part == 4 ? -0.09f : 0.14f)) * loose + rattle;
        float xform[16];
        make_rot_x(d.centre, tilt, droop + rattle * 0.4f, xform);
        multiply(xform, d.rest_model, d.push.model);
      } else {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      }
    }
  }
}

void Showroom::set_animated_car_parts(float aero_deploy, float popup_open, float time_sec, float dt) {
  const float step = std::clamp(dt * 4.5f, 0.01f, 1.0f);
  aero_deploy_smooth_ += (std::clamp(aero_deploy, 0.0f, 1.0f) - aero_deploy_smooth_) * step;
  popup_open_smooth_ += (std::clamp(popup_open, 0.0f, 1.0f) - popup_open_smooth_) * step;
  auto make_rot_x = [](const std::array<float, 3>& pivot, float angle, float dy, float out[16]) {
    const float c = std::cos(angle), s = std::sin(angle);
    identity(out);
    out[5] = c;
    out[6] = s;
    out[9] = -s;
    out[10] = c;
    const float py = pivot[1], pz = pivot[2];
    out[13] = py + dy - (c * py - s * pz);
    out[14] = pz - (s * py + c * pz);
  };
  // Compute unified AABBs for main active rear wing (`winga`) and linkage sub-parts
  // (`winga_arm*`, `winga_piston`, `winga_lower`, `winga_suspension`) so multi-subsection
  // spoilers move rigidly around one hinge pivot with physically bounded strut travel.
  bool has_winga_main = false;
  bool has_wing_linkages = false;
  std::array<float, 3> wing_min{1e9f, 1e9f, 1e9f}, wing_max{-1e9f, -1e9f, -1e9f};
  std::array<float, 3> link_min{1e9f, 1e9f, 1e9f}, link_max{-1e9f, -1e9f, -1e9f};
  for (const Draw& d : draws_) {
    if (d.wheel >= 0 || d.cockpit_role >= 0) continue;
    if (d.anim_part == 1 && d.part_name == "winga") {
      has_winga_main = true;
      for (size_t k = 0; k < 3; ++k) {
        wing_min[k] = std::min(wing_min[k], d.part_min[k]);
        wing_max[k] = std::max(wing_max[k], d.part_max[k]);
      }
    } else if (d.anim_part == 8) {
      has_wing_linkages = true;
      for (size_t k = 0; k < 3; ++k) {
        link_min[k] = std::min(link_min[k], d.part_min[k]);
        link_max[k] = std::max(link_max[k], d.part_max[k]);
      }
    }
  }
  const float wing_height_m = has_winga_main ? std::max(0.0f, wing_max[1] - wing_min[1]) : 0.0f;
  const float link_height_m = has_wing_linkages ? std::max(0.0f, link_max[1] - link_min[1]) : 0.0f;

  for (Draw& d : draws_) {
    if (d.anim_part == 0 || d.wheel >= 0 || d.cockpit_role >= 0) continue;
    if (d.anim_part == 1) {
      if (dmg_wing_detached_) continue;
      // Active rear aero spoiler / airbrake / flaps (only on CarAnimationEvents RearWing or
      // active-aero linkage cars): smoothly deploys within realistic mount travel while keeping
      // all subsections rigidly joined to the decklid hinge or linkages.
      if (aero_deploy_smooth_ > 0.002f) {
        const bool is_flap_only = !has_winga_main;
        const auto dep = cars::compute_active_wing_deploy(
            aero_deploy_smooth_, wing_height_m, link_height_m, has_wing_linkages, false, is_flap_only);
        const auto& bmin = is_flap_only ? d.part_min : wing_min;
        const auto& bmax = is_flap_only ? d.part_max : wing_max;
        const std::array<float, 3> pivot{
            0.5f * (bmin[0] + bmax[0]),
            bmin[1],
            bmin[2] + (bmax[2] - bmin[2]) * dep.pivot_z_frac,
        };
        float xform[16];
        make_rot_x(pivot, dep.pitch_rad, dep.lift_m, xform);
        multiply(xform, d.rest_model, d.push.model);
      } else {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      }
    } else if (d.anim_part == 8) {
      if (dmg_wing_detached_) continue;
      // Active rear wing linkage sub-parts (winga_arm*, winga_piston, winga_lower, winga_suspension):
      // extend smoothly with the deployed wing using the unified wing-mount pivot so arms/pistons
      // remain visually connected at both the decklid socket and the spoiler underside.
      if (aero_deploy_smooth_ > 0.002f) {
        const auto dep = cars::compute_active_wing_deploy(
            aero_deploy_smooth_, wing_height_m, link_height_m, true, true, false);
        const std::array<float, 3> pivot{
            0.5f * (link_min[0] + link_max[0]),
            link_min[1],
            link_min[2] + (link_max[2] - link_min[2]) * dep.pivot_z_frac,
        };
        float xform[16];
        make_rot_x(pivot, dep.pitch_rad, dep.lift_m, xform);
        multiply(xform, d.rest_model, d.push.model);
      } else {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      }
    } else if (d.anim_part == 7) {
      // Active EngineVent louvers (e.g. Murcielago SV, Aventador, Reventon): tilt open at speed.
      if (aero_deploy_smooth_ > 0.002f) {
        float xform[16];
        make_rot_x(d.centre, aero_deploy_smooth_ * 0.26f, aero_deploy_smooth_ * 0.025f, xform);
        multiply(xform, d.rest_model, d.push.model);
      } else {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      }
    } else if (d.anim_part == 2) {
      // Retractable pop-up headlight pods: hinge front lip upward (+X rotation in -Z forward model frame)
      // when headlamps activate.
      if (popup_open_smooth_ > 0.002f) {
        float xform[16];
        make_rot_x(d.centre, popup_open_smooth_ * 0.55f, popup_open_smooth_ * 0.045f, xform);
        multiply(xform, d.rest_model, d.push.model);
      } else {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      }
    } else if (d.anim_part == 9 || d.anim_part == 10) {
      // Dual-mesh pop-up headlights (headlightLup/Rup vs headlightLdown/Rdown, e.g. BMW M1):
      // toggle cleanly between retracted and deployed pod meshes.
      const bool show = (d.anim_part == 9) ? (popup_open_smooth_ >= 0.5f) : (popup_open_smooth_ < 0.5f);
      if (show) {
        std::memcpy(d.push.model, d.rest_model, sizeof d.push.model);
      } else {
        std::memset(d.push.model, 0, sizeof d.push.model);
        d.push.model[15] = 1.0f;
      }
    }
  }
  apply_damage_part_transforms(time_sec);
}

void Showroom::set_lamp_state(const drive::CarLampState& state) {
  lamp_vectors_ = state.vectors();
  lamp_state_active_ = true;
  cockpit_mfd_ext_[3] = std::clamp(lamp_vectors_.dynamic_lights_2[0], 0.0f, 1.0f);
  frame_.cockpit_mfd_ext[3] = cockpit_mfd_ext_[3];
}

void Showroom::set_lamp_vectors(const drive::LampShaderVectors& vectors) {
  lamp_vectors_ = vectors;
  lamp_state_active_ = true;
  cockpit_mfd_ext_[3] = std::clamp(lamp_vectors_.dynamic_lights_2[0], 0.0f, 1.0f);
  frame_.cockpit_mfd_ext[3] = cockpit_mfd_ext_[3];
}

void Showroom::set_cockpit_wheel_offsets(std::array<float, 3> pos_offset_m, float rot_offset_deg) {
  const bool has_pos = (std::fabs(pos_offset_m[0]) + std::fabs(pos_offset_m[1]) + std::fabs(pos_offset_m[2])) > 1e-6f;
  cockpit_wheel_pos_offset_ = has_pos ? pos_offset_m : car_attribs_wheel_pos_;
  cockpit_wheel_rot_offset_rad_ =
      std::fabs(rot_offset_deg) > 1e-5f
          ? (rot_offset_deg * (3.14159265358979323846f / 180.0f))
          : car_attribs_wheel_rot_rad_;
}

void Showroom::set_cockpit_telemetry(const CockpitTelemetry& t) {
  const float disp_speed = std::max(0.0f, t.speed_mps) * (t.metric_units ? 3.6f : 2.23693629f);
  const bool shift_led = cockpit_mfd_[1] >= 0.90f && t.gear >= 1;
  uint32_t mflags = 256u;  // live MFD telemetry active
  if (t.metric_units) mflags |= 1u;
  if (t.abs_active) mflags |= 2u;
  if (t.tcs_active) mflags |= 4u;
  if (t.handbrake_active) mflags |= 8u;
  if (t.high_beam) mflags |= 16u;
  if (t.turn_left) mflags |= 32u;
  if (t.turn_right) mflags |= 64u;
  if (shift_led) mflags |= 128u;
  cockpit_mfd_ext_[0] = float(std::clamp(t.gear, -1, 8));
  cockpit_mfd_ext_[1] = disp_speed;
  cockpit_mfd_ext_[2] = float(mflags);
  cockpit_mfd_ext_[3] = std::clamp(lamp_vectors_.dynamic_lights_2[0], 0.0f, 1.0f);
  for (int k = 0; k < 4; ++k) frame_.cockpit_mfd_ext[k] = cockpit_mfd_ext_[k];

  // Smooth gear-shift reach and handbrake pull driver hand animations.
  const float safe_dt = std::clamp(t.dt, 0.0f, 0.15f);
  if (t.gear != driver_last_gear_ && t.gear != 0) {
    driver_shift_timer_ = 0.36f;
    driver_last_gear_ = t.gear;
  } else if (t.shifting && driver_shift_timer_ < 0.22f) {
    driver_shift_timer_ = 0.22f;
  }
  if (driver_shift_timer_ > 0.0f) {
    driver_shift_timer_ = std::max(0.0f, driver_shift_timer_ - safe_dt);
    const float u = 1.0f - (driver_shift_timer_ / 0.36f);
    // Smooth attack (0..0.3), hold (0.3..0.65), release (0.65..1.0).
    if (u < 0.30f) driver_shift_blend_ = std::clamp(u / 0.30f, 0.0f, 1.0f);
    else if (u < 0.65f) driver_shift_blend_ = 1.0f;
    else driver_shift_blend_ = std::clamp((1.0f - u) / 0.35f, 0.0f, 1.0f);
  } else {
    driver_shift_blend_ = std::max(0.0f, driver_shift_blend_ - safe_dt * 6.0f);
  }
  const float hb_target = t.handbrake_active ? 1.0f : 0.0f;
  driver_handbrake_blend_ += (hb_target - driver_handbrake_blend_) * std::clamp(safe_dt * 12.0f, 0.0f, 1.0f);
}

void Showroom::set_cockpit_anim(const drive::CockpitAnimState& anim, bool cockpit_camera) {
  cockpit_camera_active_ = cockpit_camera;
  update_mirror_mode();
  cockpit_mfd_[0] = std::clamp(anim.speed_norm, 0.0f, 1.0f);
  cockpit_mfd_[1] = std::clamp(anim.tach_norm, 0.0f, 1.0f);
  cockpit_mfd_[2] = std::clamp(anim.fuel_norm, 0.0f, 1.0f);
  cockpit_mfd_[3] = std::clamp(std::max(anim.boost_norm, anim.power_norm), 0.0f, 1.0f);
  for (int k = 0; k < 4; ++k) frame_.cockpit_mfd[k] = cockpit_mfd_[k];
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
        const float bf = world::wheel_blur_factor(p.omega);
        d.push.params[3] = bf;
        if (d.is_blur_rim) {
          const int tier = std::clamp(int(bf * 3.99f), 0, 3);
          for (int t = tier; t >= 0; --t) {
            if (d.blur_descriptors[size_t(t)] != UINT32_MAX) {
              d.descriptor = d.blur_descriptors[size_t(t)];
              break;
            }
          }
        }
      }
    }
  }
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
  // Resolve per-color ShaderFinishParams (ColorShaderSettings<seq>.xml -> Shared Metallic/Normal).
  active_paint_params_ = c.metallic ? shader_settings_.metallic : shader_settings_.normal;
  if (auto it = car_shader_overrides_.color_overrides.find(int(c.sequence));
      it != car_shader_overrides_.color_overrides.end()) {
    active_paint_params_ = it->second;
  } else if (auto it2 = car_shader_overrides_.color_overrides.find(int(index + 1));
             it2 != car_shader_overrides_.color_overrides.end()) {
    active_paint_params_ = it2->second;
  }
  paint_finish_[0] = c.metallic ? 1.0f : 0.0f;
  for (int r = 0; r < 3; ++r) region_finish_[r][3] = 0.0f;
  frame_.paint_finish[0] = paint_finish_[0];
  for (int r = 0; r < 3; ++r) frame_.region_finish[r][3] = 0.0f;
  sync_paint_shader_uniforms();

  // Detect alternate factory livery atlas (Combo_Colors.BaseTexture differing from standard nodamage).
  const std::string lod0_l = lower(c.atlas_lod0);
  const std::string lod_l = lower(c.atlas_lod);
  const bool alt_livery_tex =
      (!lod0_l.empty() && lod0_l.find("livery") != std::string::npos) ||
      (!lod_l.empty() && lod_l.find("livery") != std::string::npos);
  const bool is_race_liv = (stock_ && stock_->race_livery) || alt_livery_tex;
  auto lookup_atlas_idx = [&](bool want_lod0) -> uint32_t {
    const std::string& mem = (want_lod0 && !c.atlas_lod0.empty())
                                 ? c.atlas_lod0
                                 : (!c.atlas_lod.empty() ? c.atlas_lod : c.atlas_lod0);
    if (mem.empty()) return UINT32_MAX;
    auto it = texture_by_key_.find("car:" + mem);
    return (it != texture_by_key_.end()) ? it->second : UINT32_MAX;
  };
  const uint32_t alt_lod0_tex = lookup_atlas_idx(true);
  const uint32_t alt_lod_tex = lookup_atlas_idx(false);
  const float pscale = std::clamp(active_paint_params_.paint_scale, 0.15f, 1.85f);

  for (auto& d : draws_) {
    if (d.uses_atlas) {
      const uint32_t want_t0 = d.atlas_lod0 ? alt_lod0_tex : alt_lod_tex;
      if (want_t0 != UINT32_MAX && want_t0 != d.textures[0]) {
        std::array<uint32_t, 5> alt_key = d.textures;
        alt_key[0] = want_t0;
        if (auto dit = descriptor_by_textures_.find(alt_key); dit != descriptor_by_textures_.end()) {
          d.textures[0] = want_t0;
          d.descriptor = dit->second;
        }
      }
    }
    if (uint32_t(d.push.params[0] + 0.5f) != uint32_t(MaterialClass::kPaint)) continue;
    uint32_t flags = uint32_t(std::lround(d.push.params[1]));
    for (int k = 0; k < 3; ++k) d.push.tint[k] = rgb[k];
    d.push.tint[3] = pscale;
    d.push.params[3] = 0.0f;
    flags = c.metallic ? (flags | kFlagMetallic) : (flags & ~kFlagMetallic);
    flags = (c.has_stripe && d.stripe_able && !is_race_liv) ? (flags | kFlagStripe) : (flags & ~kFlagStripe);
    flags = is_race_liv ? (flags | kFlagRaceLivery) : (flags & ~kFlagRaceLivery);
    d.push.params[1] = float(flags);
  }
}

void Showroom::set_paint_spec(const cars::CarPaintSpec& paint) {
  auto to_lin = [](uint32_t rgb) -> std::array<float, 3> {
    return {srgb_to_linear(float((rgb >> 16) & 0xFF) / 255.0f),
            srgb_to_linear(float((rgb >> 8) & 0xFF) / 255.0f),
            srgb_to_linear(float(rgb & 0xFF) / 255.0f)};
  };
  const auto& exc = options_.visual.paint_exceptions;
  if (paint.body.active) {
    const auto sec = to_lin(paint.body.sec_rgb);
    paint_two_tone_[0] = sec[0];
    paint_two_tone_[1] = sec[1];
    paint_two_tone_[2] = sec[2];
    paint_two_tone_[3] = paint.body.two_tone_power;
    paint_finish_[0] = float(int(paint.body.finish));
    paint_finish_[1] = paint.body.two_tone_scale;
    paint_finish_[2] = paint.body.two_tone_bias;
    switch (paint.body.finish) {
      case cars::PaintFinish::kMetallic:
      case cars::PaintFinish::kTwoTone:
        active_paint_params_ = shader_settings_.metallic;
        break;
      case cars::PaintFinish::kMatte:
        active_paint_params_ = shader_settings_.matte;
        break;
      default:
        active_paint_params_ = shader_settings_.normal;
        break;
    }
    sync_paint_shader_uniforms();
  }
  // Per-region paint finish & two-tone overrides (0=hood, 1=wing, 2=mirrors), respecting CarExceptions.
  const cars::PaintRegionSpec* regs[3] = {&paint.hood, &paint.wing, &paint.mirrors};
  const bool reg_blocked[3] = {exc.no_hood, exc.no_paintable_wing, exc.no_mirrors};
  for (int r = 0; r < 3; ++r) {
    if (regs[r]->active && !reg_blocked[r]) {
      const auto sec = to_lin(regs[r]->sec_rgb);
      region_two_tone_[r][0] = sec[0];
      region_two_tone_[r][1] = sec[1];
      region_two_tone_[r][2] = sec[2];
      region_two_tone_[r][3] = regs[r]->two_tone_power;
      region_finish_[r][0] = float(int(regs[r]->finish));
      region_finish_[r][1] = regs[r]->two_tone_scale;
      region_finish_[r][2] = regs[r]->two_tone_bias;
      region_finish_[r][3] = 1.0f;
    } else {
      region_finish_[r][3] = 0.0f;
    }
    for (int i = 0; i < 4; ++i) {
      frame_.region_two_tone[r][i] = region_two_tone_[r][i];
      frame_.region_finish[r][i] = region_finish_[r][i];
    }
  }
  paint_finish_[3] = exc.no_windows ? 0.0f : std::clamp(paint.window_tint, 0.0f, 1.0f);
  for (int i = 0; i < 4; ++i) {
    frame_.paint_two_tone[i] = paint_two_tone_[i];
    frame_.paint_finish[i] = paint_finish_[i];
  }

  auto paint_scale_for = [&](cars::PaintFinish f) -> float {
    if (!shader_settings_.loaded_from_xml) return 1.0f;
    switch (f) {
      case cars::PaintFinish::kMetallic:
      case cars::PaintFinish::kTwoTone:
        return std::clamp(shader_settings_.metallic.paint_scale, 0.15f, 1.85f);
      case cars::PaintFinish::kMatte:
        return std::clamp(shader_settings_.matte.paint_scale, 0.15f, 1.85f);
      default:
        return std::clamp(shader_settings_.normal.paint_scale, 0.15f, 1.85f);
    }
  };
  for (auto& d : draws_) {
    const auto klass = MaterialClass(uint32_t(d.push.params[0] + 0.5f));
    if (klass == MaterialClass::kPaint) {
      const cars::PaintRegionSpec* reg = &paint.body;
      bool per_region = false;
      if (!exc.no_hood && paint.hood.active && d.part_name.rfind("hood", 0) == 0) {
        reg = &paint.hood;
        per_region = true;
      } else if (!exc.no_paintable_wing && paint.wing.active && d.part_name.rfind("wing", 0) == 0) {
        reg = &paint.wing;
        per_region = true;
      } else if (!exc.no_mirrors && paint.mirrors.active && d.part_name.find("mirror") != std::string::npos) {
        reg = &paint.mirrors;
        per_region = true;
      }
      if (reg->active) {
        const auto lin = to_lin(reg->rgb);
        for (int k = 0; k < 3; ++k) d.push.tint[k] = lin[k];
        d.push.tint[3] = paint_scale_for(reg->finish);
        uint32_t flags = uint32_t(std::lround(d.push.params[1]));
        const bool is_met = (reg->finish == cars::PaintFinish::kMetallic || reg->finish == cars::PaintFinish::kTwoTone);
        flags = is_met ? (flags | kFlagMetallic) : (flags & ~kFlagMetallic);
        flags &= ~(kFlagRaceLivery | kFlagStripe);
        d.push.params[1] = float(flags);
        // Encode per-region finish override (hood/wing/mirrors) in params[3] (+1 so 0 means body uniform).
        d.push.params[3] = per_region ? float(int(reg->finish) + 1) : 0.0f;
      }
    } else if (klass == MaterialClass::kRim && paint.rims.active &&
               !options_.visual.wheel_is_chrome && !options_.visual.wheel_is_stock &&
               !options_.visual.wheel_is_exception) {
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
    uint32_t flags = uint32_t(std::lround(d.push.params[1]));
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
  // Dynamic environmental body/tyre dirt & mud stay active in ALL damage modes (including None).
  damage_dirt_[0] = std::clamp(dmg.body_dirt, 0.0f, 1.0f);
  damage_dirt_[1] = std::clamp(dmg.tyre_dirt, 0.0f, 1.0f);
  if (dmg.mode == drive::DamageMode::kNone) {
    damage_dirt_[2] = 0.0f;
    damage_dirt_[3] = 0.0f;
    for (int i = 0; i < 4; ++i) damage_zones_[i] = damage_corners_[i] = 0.0f;
    dmg_front_bumper_detached_ = false;
    dmg_rear_bumper_detached_ = false;
    dmg_left_mirror_detached_ = false;
    dmg_right_mirror_detached_ = false;
    dmg_wing_detached_ = false;
    dmg_hood_ajar_ = 0.0f;
  } else {
    damage_dirt_[2] = std::clamp(std::max(dmg.glass_crack, dmg.zone_roof), 0.0f, 1.0f);
    damage_dirt_[3] = std::clamp(dmg.scratch_amount, 0.0f, 1.0f);
    damage_zones_[0] = std::clamp(dmg.zone_front, 0.0f, 1.0f);
    damage_zones_[1] = std::clamp(dmg.zone_rear, 0.0f, 1.0f);
    damage_zones_[2] = std::clamp(dmg.zone_left, 0.0f, 1.0f);
    damage_zones_[3] = std::clamp(dmg.zone_right, 0.0f, 1.0f);
    damage_corners_[0] = std::clamp(dmg.corner_fl, 0.0f, 1.0f);
    damage_corners_[1] = std::clamp(dmg.corner_fr, 0.0f, 1.0f);
    damage_corners_[2] = std::clamp(dmg.corner_rl, 0.0f, 1.0f);
    damage_corners_[3] = std::clamp(dmg.corner_rr, 0.0f, 1.0f);
    dmg_front_bumper_detached_ = dmg.front_bumper_detached;
    dmg_rear_bumper_detached_ = dmg.rear_bumper_detached;
    dmg_left_mirror_detached_ = dmg.left_mirror_detached;
    dmg_right_mirror_detached_ = dmg.right_mirror_detached;
    dmg_wing_detached_ = dmg.wing_detached;
    dmg_hood_ajar_ = std::clamp(dmg.hood_ajar, 0.0f, 1.0f);
  }
  for (int i = 0; i < 4; ++i) {
    frame_.damage_dirt[i] = damage_dirt_[i];
    frame_.damage_zones[i] = damage_zones_[i];
    frame_.damage_corners[i] = damage_corners_[i];
  }
  apply_damage_part_transforms(0.0f);
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
  GpuTexture new_car_dirt_mask{};
  bool new_has_car_dirt_mask = false;
  const bake::Texture* car_dm_tex = nullptr;
  for (const auto& tx : car_.textures) {
    const std::string tn = lower(tx.name);
    if (tn == "dirt_mask.xds" || tn == "dirt_mask_lod0.xds") {
      car_dm_tex = &tx;
      break;
    }
  }
  if (car_dm_tex) {
    batch.push_back({car_dm_tex, false, &new_car_dirt_mask, false});
    new_has_car_dirt_mask = true;
  }
  if (!upload_textures_batch(batch, error)) {
    r_.destroy_image(new_car_dirt_mask);
    r_.destroy_buffer(new_vb);
    r_.destroy_buffer(new_ib);
    return false;
  }
  if (gpu_textures_.size() >= 2 && new_gpu_textures.size() >= 2) {
    new_gpu_textures[0] = gpu_textures_[0];
    new_gpu_textures[1] = gpu_textures_[1];
  }

  // Create fresh material descriptor pool & sets for the new car draws,
  // plus pre-allocated sets for alternate factory liveries and blur_rim wheel0..3.xds tiers.
  std::vector<uint32_t> alt_atlas_indices;
  for (const auto& col : car_.car.colours) {
    for (const std::string& m : {col.atlas_lod0, col.atlas_lod}) {
      if (m.empty()) continue;
      if (auto it = texture_by_key_.find("car:" + m); it != texture_by_key_.end() && it->second != UINT32_MAX) {
        if (std::find(alt_atlas_indices.begin(), alt_atlas_indices.end(), it->second) == alt_atlas_indices.end()) {
          alt_atlas_indices.push_back(it->second);
        }
      }
    }
  }
  std::array<uint32_t, 4> wheel_blur_tex{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  {
    const char* const kWheelBlurNames[4] = {"wheel:wheel0.xds", "wheel:wheel1.xds", "wheel:wheel2.xds", "wheel:wheel3.xds"};
    for (int t = 0; t < 4; ++t) {
      if (auto it = texture_by_key_.find(kWheelBlurNames[t]); it != texture_by_key_.end()) {
        wheel_blur_tex[size_t(t)] = it->second;
      }
    }
  }
  for (auto& d : draws_) {
    auto [it, inserted] = descriptor_by_textures_.emplace(d.textures, uint32_t(descriptor_by_textures_.size()));
    d.descriptor = it->second;
    if (d.uses_atlas) {
      for (uint32_t alt_t0 : alt_atlas_indices) {
        if (alt_t0 == d.textures[0]) continue;
        std::array<uint32_t, 5> alt_k = d.textures;
        alt_k[0] = alt_t0;
        descriptor_by_textures_.emplace(alt_k, uint32_t(descriptor_by_textures_.size()));
      }
    }
    if (d.is_blur_rim) {
      for (int t = 0; t < 4; ++t) {
        if (wheel_blur_tex[size_t(t)] != UINT32_MAX) {
          std::array<uint32_t, 5> bk = d.textures;
          bk[1] = wheel_blur_tex[size_t(t)];
          auto [bit, binserted] = descriptor_by_textures_.emplace(bk, uint32_t(descriptor_by_textures_.size()));
          d.blur_descriptors[size_t(t)] = bit->second;
        }
      }
    }
  }
  for (auto& d : far_draws_) {
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
    r_.destroy_image(new_car_dirt_mask);
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
      r_.destroy_image(new_car_dirt_mask);
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
  GpuTexture old_car_dirt = car_dirt_mask_;
  std::vector<GpuTexture> old_car_textures;
  if (gpu_textures_.size() > 2) {
    old_car_textures.assign(gpu_textures_.begin() + 2, gpu_textures_.end());
  }

  vertex_buffer_ = new_vb;
  index_buffer_ = new_ib;
  material_pool_ = new_mat_pool;
  material_sets_ = std::move(new_mat_sets);
  gpu_textures_ = std::move(new_gpu_textures);
  car_dirt_mask_ = new_car_dirt_mask;
  has_car_dirt_mask_ = new_has_car_dirt_mask && (car_dirt_mask_.view != VK_NULL_HANDLE);
  custom_livery_img_ = {};
  custom_livery_active_ = false;
  lamp_points_.clear();
  lamp_state_active_ = false;
  lamp_vectors_ = {};
  driver_wheel_rad_ = 0.0f;
  cockpit_camera_active_ = false;
  update_driver_mesh(0.0f);
  // Rebind frame_set_ binding 9 to the new car's dirt_mask.xds (or Shared fallback) via a fresh set from pool_.
  if (pool_ && frame_layout_ && sampler_ && shared_dirt_mask_.view) {
    VkDescriptorSet fresh = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo fai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    fai.descriptorPool = pool_;
    fai.descriptorSetCount = 1;
    fai.pSetLayouts = &frame_layout_;
    if (vkAllocateDescriptorSets(r_.device_, &fai, &fresh) == VK_SUCCESS && fresh) {
      VkDescriptorBufferInfo buffer{frame_buffer_.buffer, 0, sizeof(ShowroomFrame)};
      VkImageView cube_view = bound_dyn_env_view_
                                  ? bound_dyn_env_view_
                                  : ((dyn_env_bound_ && dyn_env_.view) ? dyn_env_.view : environment_.view);
      VkDescriptorImageInfo cube{sampler_, cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo static_cube{sampler_, environment_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo map{r_.shadow_sampler_, r_.shadow_map_.view,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo map_raw{r_.shadow_raw_sampler_, r_.shadow_map_.view,
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo hl_buf{headlights_ubo_.buffer, 0, sizeof(HeadlightUbo)};
      const VkImageView mirror_view =
          (dyn_env_ready_ && mirror_tex_.view) ? mirror_tex_.view : gpu_textures_[1].view;
      VkDescriptorImageInfo mir{sampler_, mirror_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo inst_buf{guest_ ? empty_inst_ssbo_.buffer : car_inst_buf_.buf.buffer, 0, VK_WHOLE_SIZE};
      VkDescriptorImageInfo dirt_p{sampler_, shared_dirt_pattern_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      const VkImageView dm_view = has_car_dirt_mask_ ? car_dirt_mask_.view : shared_dirt_mask_.view;
      VkDescriptorImageInfo dirt_m{sampler_, dm_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
      if (frame_set_dip_) frame_set_dip_ = fresh;
      sync_fullbeam_frame_set(fresh, cube_view, sampler_);
    }
  }
  sync_paint_shader_uniforms();
  update_mirror_mode();
  // Reset per-frame car-derived uniforms so the new car's stripe/paint finish take effect immediately.
  for (int i = 0; i < 4; ++i) {
    frame_.stripe[i] = stripe_[i];
    frame_.paint_two_tone[i] = paint_two_tone_[i];
    frame_.paint_finish[i] = paint_finish_[i];
    frame_.damage_dirt[i] = damage_dirt_[i];
    frame_.damage_zones[i] = damage_zones_[i];
    frame_.damage_corners[i] = damage_corners_[i];
  }
  frame_.dyn_light_meta[1] = rotor_glow_front_;
  frame_.dyn_light_meta[2] = rotor_glow_rear_;
  for (int r = 0; r < 3; ++r) {
    for (int i = 0; i < 4; ++i) {
      frame_.region_two_tone[r][i] = region_two_tone_[r][i];
      frame_.region_finish[r][i] = region_finish_[r][i];
    }
  }
  frame_.options[2] = 0.0f;

  retire([this, old_vb, old_ib, old_mat_pool, old_livery, old_car_dirt,
          old_tex = std::move(old_car_textures)]() mutable {
    if (old_mat_pool) vkDestroyDescriptorPool(r_.device_, old_mat_pool, nullptr);
    for (auto& t : old_tex) r_.destroy_image(t);
    r_.destroy_image(old_livery);
    r_.destroy_image(old_car_dirt);
    r_.destroy_buffer(old_vb);
    r_.destroy_buffer(old_ib);
  });
  return true;
}


}  // namespace carview
