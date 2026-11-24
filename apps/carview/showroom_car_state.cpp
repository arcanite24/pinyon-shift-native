#include "showroom_internal.h"

#include "cars/bake.h"
#include "cars/texture_mods.h"

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
      d.push.tint[3] = 1;
      // Sub-material variant ID encoded in params[2] for showroom.frag:
      // 1=outer_rim (chrome lip), 2=inner_rim/wheel_black (dark barrel), 3=rotor/brake/drum,
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
      } else if (klass == MaterialClass::kGlass && (flags & kFlagCover) == 0u) {
        const std::string pname_l = lower(part.name);
        if (pname_l == "interior" || pname_l.rfind("headlight", 0) == 0 ||
            pname_l.rfind("taillight", 0) == 0 || pname_l == "glasslhl" || pname_l == "glassrhl" ||
            pname_l == "glassltl" || pname_l == "glassrtl" ||
            technique == "lights_glass" || technique == "light_glass" || technique == "red_glass") {
          submat_id = 9.0f;
        } else if (pname_l == "glassf" || technique == "frontwindow") {
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
      d.glass = (klass == MaterialClass::kGlass);
      d.part_name = lower(part.name);
      draws_.push_back(d);
      drawn = true;
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
    damage_dirt_[2] = std::clamp(std::max(dmg.glass_crack, dmg.zone_roof), 0.0f, 1.0f);
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


}  // namespace carview
