// Unit tests for Phase R2-1 Cars & Visuals clean-room parity fixes:
// - P0-C1: Livery projection bottom-up TGA Y conversion, uniform aspect ratio,
//          Left <-> Right mirror symmetry, Masks.xml xScale/yScale/xAxis/yAxis,
//          and per-plane LiveryMasks/<plane>.tga alpha clipping.
// - P0-C2: Tire rolling outer radius preservation on rim-size & tire-width upgrades.
// - P0-C3 & P1-C5: List_UpgradeTireCompound.Level & Front/RearSpringDamper.StaticCamber
//                  SQLite queries and compound-to-texture archive mapping.
// - P0-C4: CaliperAngleFront/Rear radians, brake size 17, drum_null_caliper,
//          and Shared/rotor0..4.xds slot binding (never @atlas).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "cars/bake.h"
#include "cars/database.h"
#include "cars/livery.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "cars/visual_setup.h"
#include "fh1data/archive.h"

namespace {

int g_failed = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++g_failed;                                                                        \
    }                                                                                    \
  } while (0)

namespace fs = std::filesystem;

fs::path find_game_root() {
  for (const char* p : {R"(../horizon1-recomp/.local\game\base)",
                        R"(.local\game\base)"}) {
    if (fs::exists(fs::path(p) / "media" / "db" / "gamedb.slt")) return p;
  }
  return {};
}

// Measures pixel bounding box and alpha-weighted centroid of non-zero pixels inside a region.
struct RegionStats {
  int min_x = 100000, max_x = -1, min_y = 100000, max_y = -1;
  double cx = 0.0, cy = 0.0;
  int count = 0;
};

RegionStats measure_region(const std::vector<uint8_t>& rgba, uint32_t size, int y0, int y1) {
  RegionStats s;
  double sum_a = 0.0;
  for (int y = std::max(0, y0); y < std::min(int(size), y1); ++y) {
    for (int x = 0; x < int(size); ++x) {
      const uint8_t a = rgba[(size_t(y) * size + size_t(x)) * 4u + 3u];
      if (a > 16) {
        s.min_x = std::min(s.min_x, x);
        s.max_x = std::max(s.max_x, x);
        s.min_y = std::min(s.min_y, y);
        s.max_y = std::max(s.max_y, y);
        s.cx += double(x) * double(a);
        s.cy += double(y) * double(a);
        sum_a += double(a);
        ++s.count;
      }
    }
  }
  if (sum_a > 0.0) {
    s.cx /= sum_a;
    s.cy /= sum_a;
  }
  return s;
}

void test_livery_projection_yflip_aspect_mirror_and_mask() {
  // Authentic Masks.xml layout (bottom-up TGA coordinates around (512, 512)):
  // Left  (866x214): left=-377, right=489, top=-506, bottom=-292, rot=0
  //   -> top-first UV: u=[135..1001]/1024, v=[804..1018]/1024
  // Right (866x215): left=-377, right=489, top=73,   bottom=288,  rot=180
  //   -> top-first UV: u=[135..1001]/1024, v=[224..439]/1024
  cars::bake::LiveryPlane left_p;
  left_p.name = "Left";
  left_p.valid = true;
  left_p.left = -377.0f;
  left_p.right = 489.0f;
  left_p.top = -506.0f;
  left_p.bottom = -292.0f;
  left_p.x_axis = "-z";
  left_p.y_axis = "+y";
  left_p.x_scale = 1.0f;
  left_p.y_scale = 1.0f;
  left_p.rotation = 0.0f;

  cars::bake::LiveryPlane right_p;
  right_p.name = "Right";
  right_p.valid = true;
  right_p.left = -377.0f;
  right_p.right = 489.0f;
  right_p.top = 73.0f;
  right_p.bottom = 288.0f;
  right_p.x_axis = "-z";
  right_p.y_axis = "-y";
  right_p.x_scale = 1.0f;
  right_p.y_scale = 1.0f;
  right_p.rotation = 180.0f;

  std::vector<cars::bake::LiveryPlane> planes{left_p, right_p};

  // 1. Verify bottom-up TGA Y -> top-first Vulkan UV conversion.
  const auto rl = cars::resolve_livery_plane_rect(&planes, cars::LiveryPlaneKind::kLeft);
  const auto rr = cars::resolve_livery_plane_rect(&planes, cars::LiveryPlaneKind::kRight);
  CHECK(std::fabs(rl.u0 - 135.0f / 1024.0f) < 1e-4f);
  CHECK(std::fabs(rl.v0 - 804.0f / 1024.0f) < 1e-4f);
  CHECK(std::fabs(rl.w - 866.0f / 1024.0f) < 1e-4f);
  CHECK(std::fabs(rl.h - 214.0f / 1024.0f) < 1e-4f);
  CHECK(std::fabs(rr.v0 - 224.0f / 1024.0f) < 1e-4f);
  CHECK(std::fabs(rr.h - 215.0f / 1024.0f) < 1e-4f);

  // 2. Verify aspect-ratio preservation: circular decal on ~4:1 side plane must stay circular (~1:1).
  {
    cars::LiveryDocument doc;
    cars::VinylLayer circle;
    circle.shape_path = "@circle";
    circle.plane = cars::LiveryPlaneKind::kLeft;
    circle.x = 0.5f;
    circle.y = 0.5f;
    circle.scale_x = 0.6f;
    circle.scale_y = 0.6f;
    circle.rgb = 0xFF3300u;
    doc.layers.push_back(circle);

    std::vector<uint8_t> rgba;
    CHECK(cars::composite_livery(doc, nullptr, &planes, rgba, 1024));
    const RegionStats st = measure_region(rgba, 1024, 800, 1024);
    CHECK(st.count > 1000);
    const float w_px = float(st.max_x - st.min_x + 1);
    const float h_px = float(st.max_y - st.min_y + 1);
    const float aspect = w_px / std::max(1.0f, h_px);
    CHECK(aspect > 0.92f && aspect < 1.08f);
  }

  // 3. Verify upright orientation and Left <-> Right mirror symmetry.
  // Place an upward chevron near front-top on Left (x=0.25, y=0.30) and its mirror on Right (x=0.75, y=0.30).
  {
    cars::LiveryDocument doc;
    cars::VinylLayer l_lay;
    l_lay.shape_path = "@chevron";
    l_lay.plane = cars::LiveryPlaneKind::kLeft;
    l_lay.x = 0.25f;
    l_lay.y = 0.30f;
    l_lay.scale_x = 0.45f;
    l_lay.scale_y = 0.45f;
    l_lay.rgb = 0xFFFFFFu;
    doc.layers.push_back(l_lay);

    cars::VinylLayer r_lay = l_lay;
    r_lay.plane = cars::LiveryPlaneKind::kRight;
    r_lay.x = 1.0f - l_lay.x;
    r_lay.flip_x = !l_lay.flip_x;
    doc.layers.push_back(r_lay);

    std::vector<uint8_t> rgba;
    CHECK(cars::composite_livery(doc, nullptr, &planes, rgba, 1024));
    const RegionStats sl = measure_region(rgba, 1024, 804, 1018);
    const RegionStats sr = measure_region(rgba, 1024, 224, 439);
    CHECK(sl.count > 100 && sr.count > 100);
    // Both chevrons must sit at the same longitudinal U coordinate (distance from car nose).
    CHECK(std::fabs(sl.cx - sr.cx) < 2.5);
    // On Left (rot=0), roof is at y=804 (top edge); on Right (rot=180), roof is at y=439 (bottom edge).
    // Both decals must be equally close to their plane's roof edge!
    const double l_dist_from_roof = sl.cy - 804.0;
    const double r_dist_from_roof = 439.0 - sr.cy;
    CHECK(std::fabs(l_dist_from_roof - r_dist_from_roof) < 3.0);
  }

  // 4. Verify LiveryMasks/<plane>.tga alpha mask clipping.
  {
    cars::LiveryMaskSet masks;
    cars::LiveryPlaneMask lm;
    lm.plane_name = "left";
    lm.width = 256;
    lm.height = 256;
    lm.alpha.assign(256u * 256u, 0u);
    // Paintable only on the right half (x >= 128) of the atlas.
    for (uint32_t y = 0; y < 256; ++y)
      for (uint32_t x = 128; x < 256; ++x) lm.alpha[size_t(y) * 256u + x] = 255u;
    masks.planes.push_back(std::move(lm));

    cars::LiveryDocument doc;
    cars::VinylLayer rect;
    rect.shape_path = "@rect";
    rect.plane = cars::LiveryPlaneKind::kLeft;
    rect.x = 0.5f;
    rect.y = 0.5f;
    rect.scale_x = 1.0f;
    rect.scale_y = 1.0f;
    rect.rgb = 0x00FF00u;
    doc.layers.push_back(rect);

    std::vector<uint8_t> rgba;
    CHECK(cars::composite_livery(doc, nullptr, &planes, rgba, 256, &masks));
    // Left of mask boundary (x=120, inside plane) must be clipped to alpha=0.
    CHECK(rgba[(228u * 256u + 120u) * 4u + 3u] == 0u);
    // Inside mask boundary (x=140, inside plane) must be opaque alpha=255.
    CHECK(rgba[(228u * 256u + 140u) * 4u + 3u] == 255u);
  }
}

void test_tire_rolling_radius_and_compound_mapping() {
  // Stock Corrado-like tyre: 15" rim, 195/50 -> r_rim = 0.1905 m, sidewall = 0.0975 m, outer = 0.2880 m.
  const float stock_r = cars::compute_tire_outer_radius_m(15, 195, 50, 15, 195);
  CHECK(std::fabs(stock_r - 0.2880f) < 1e-4f);

  // Upgrading rim size to 18" and tyre width to 225 mm must preserve stock rolling outer radius!
  const float upg_r = cars::compute_tire_outer_radius_m(15, 195, 50, 18, 225);
  CHECK(std::fabs(upg_r - stock_r) < 1e-4f);

  // Extreme rim upgrade (21" -> r_rim = 0.2667 m) clamps cleanly to minimum 25 mm sidewall so rim never protrudes.
  const float extreme_r = cars::compute_tire_outer_radius_m(15, 195, 50, 21, 225);
  const float r21 = 21.0f * 0.0254f * 0.5f;
  CHECK(extreme_r >= r21 + 0.0249f);
  CHECK(extreme_r <= r21 + 0.026f);

  // Compound level -> TireA / TireB / TireC archive letter mapping.
  CHECK(cars::tire_compound_texture_letter(0) == 'a');  // Stock
  CHECK(cars::tire_compound_texture_letter(1) == 'a');  // Street
  CHECK(cars::tire_compound_texture_letter(2) == 'b');  // Sport
  CHECK(cars::tire_compound_texture_letter(3) == 'c');  // Race
  CHECK(cars::tire_compound_texture_letter(5) == 'c');  // Rally fallback
}

void test_disc_tire_compound_camber_and_brakes() {
  const fs::path game_root = find_game_root();
  if (game_root.empty()) {
    std::printf("  (skipping disc parity checks: base game_root not found)\n");
    return;
  }
  std::string err;
  cars::Install install;
  CHECK(cars::discover_install(game_root, {}, {}, install, &err));
  cars::PartsDb db;
  CHECK(db.open(install, &err));

  // 1. Verify List_UpgradeTireCompound.Level (not typo'd List_UpgradeCarBodyTireCompound)
  //    and Front/RearSpringDamper.StaticCamber (not typo'd DefCamber).
  std::vector<cars::PartRow> opts;
  CHECK(db.options(309, {}, opts, &err));  // VW Corrado (309)
  int64_t sport_tire_id = -1, race_tire_id = -1;
  for (const auto& o : opts) {
    if (o.table == "List_UpgradeTireCompound") {
      if (o.level == 2) sport_tire_id = o.id;
      if (o.level == 3) race_tire_id = o.id;
    }
  }
  CHECK(sport_tire_id > 0 && race_tire_id > 0);

  cars::FittedParts fit_sport, fit_race;
  fit_sport["List_UpgradeTireCompound"] = sport_tire_id;
  fit_race["List_UpgradeTireCompound"] = race_tire_id;
  cars::VisualSetup vis_sport, vis_race;
  CHECK(cars::resolve_visual_setup(db, 309, fit_sport, vis_sport, &err));
  CHECK(cars::resolve_visual_setup(db, 309, fit_race, vis_race, &err));
  CHECK(vis_sport.tire_compound_level == 2);
  CHECK(vis_race.tire_compound_level == 3);
  CHECK(cars::tire_compound_texture_letter(vis_sport.tire_compound_level) == 'b');
  CHECK(cars::tire_compound_texture_letter(vis_race.tire_compound_level) == 'c');

  // Stock StaticCamber on Corrado (309) is non-zero negative (e.g. -0.5 deg -> -0.0087 rad).
  CHECK(vis_sport.camber_rad[0] < -0.001f);
  // Caliper angles from Data_Car are already in radians (~ -1.556 rad front, -1.404 rad rear).
  CHECK(std::fabs(vis_sport.caliper_angle_deg[0] - (-1.5563f)) < 0.02f);
  CHECK(std::fabs(vis_sport.caliper_angle_deg[1] - (-1.4044f)) < 0.02f);

  // Stock tyre dimensions preserved in VisualSetup even when upgraded.
  CHECK(vis_sport.stock_rim_inches[0] > 0 && vis_sport.stock_tyre_width_mm[0] > 0 &&
        vis_sport.stock_tyre_aspect[0] > 0);

  // 2. Brake size 17 acceptance, drum_null_caliper for BrakeTypeID == 1, and Shared/rotor0..4.xds slots.
  cars::bake::Model rotor17;
  CHECK(cars::load_brake_model(game_root, 17, 2, 4, true, rotor17, &err));
  CHECK(!rotor17.parts.empty());

  cars::bake::Model drum_cal;
  CHECK(cars::load_brake_model(game_root, 10, 1, 2, false, drum_cal, &err));
  CHECK(drum_cal.name == "drum_null_caliper.rmb.carbin");

  for (int type_id = 1; type_id <= 5; ++type_id) {
    cars::bake::Model rotor;
    CHECK(cars::load_brake_model(game_root, 10, type_id, 4, true, rotor, &err));
    const std::string want_slot = "shared:rotor" + std::to_string(type_id - 1) + ".xds";
    bool saw_rotor_slot = false;
    for (const auto& p : rotor.parts) {
      for (const auto& s : p.subsections) {
        CHECK(s.slots[0] != "@atlas");
        if (s.slots[0] == want_slot) saw_rotor_slot = true;
      }
    }
    CHECK(saw_rotor_slot);
  }

  cars::bake::Model cal4;
  CHECK(cars::load_brake_model(game_root, 12, 4, 4, false, cal4, &err));
  for (const auto& p : cal4.parts) {
    for (const auto& s : p.subsections) {
      CHECK(s.slots[0] != "@atlas");
    }
  }

  // 3. Load real LiveryMasks/*.tga masks from FER_F40_87.zip.
  cars::LiveryMaskSet f40_masks;
  CHECK(cars::load_livery_masks_for_car(game_root, "FER_F40_87", f40_masks, &err));
  CHECK(f40_masks.find_plane(cars::LiveryPlaneKind::kLeft) != nullptr);
  CHECK(f40_masks.find_plane(cars::LiveryPlaneKind::kRight) != nullptr);
  CHECK(f40_masks.find_plane(cars::LiveryPlaneKind::kTop) != nullptr);

  // 4. Phase R2-3A: WheelNormOffsets, WheelExceptions, CarExceptions, and Rally Lv4 bodykits.
  {
    float off_314 = 0.0f, off_333 = 0.0f;
    CHECK(cars::query_wheel_norm_offset(db, 314, off_314, &err));  // AME_TorqThrust
    CHECK(std::fabs(off_314 - 0.015f) < 1e-4f);
    CHECK(cars::query_wheel_norm_offset(db, 333, off_333, &err));  // ENK_RPF1
    CHECK(std::fabs(off_333 - 0.030f) < 1e-4f);

    bool exc_342 = false, exc_333 = true;
    CHECK(cars::query_wheel_is_exception(db, 342, exc_342, &err));  // GRA_57C in WheelExceptions
    CHECK(exc_342);
    CHECK(!cars::is_wheel_paintable(db, 342, &err));
    CHECK(cars::query_wheel_is_exception(db, 333, exc_333, &err));  // ENK_RPF1 not an exception
    CHECK(!exc_333);
    CHECK(cars::is_wheel_paintable(db, 333, &err));

    cars::FittedParts fit_wheel;
    fit_wheel["List_Wheels"] = 333;
    cars::VisualSetup vis_w;
    CHECK(cars::resolve_visual_setup(db, 309, fit_wheel, vis_w, &err));
    CHECK(std::fabs(vis_w.rim_offset_front - 0.030f) < 1e-4f);
    CHECK(std::fabs(vis_w.rim_offset_rear - 0.030f) < 1e-4f);
    CHECK(!vis_w.wheel_is_exception && vis_w.wheel_is_paintable);

    fit_wheel["List_Wheels"] = 342;
    CHECK(cars::resolve_visual_setup(db, 309, fit_wheel, vis_w, &err));
    CHECK(vis_w.wheel_is_exception && !vis_w.wheel_is_paintable);

    cars::CarPaintExceptions ex247, ex249, ex251;
    CHECK(cars::car_paint_exceptions(db, 247, ex247, &err));
    CHECK(ex247.found && ex247.no_mirrors && !ex247.no_paintable_wing);
    CHECK(cars::car_paint_exceptions(db, 249, ex249, &err));
    CHECK(ex249.found && !ex249.no_mirrors && ex249.no_paintable_wing);
    CHECK(cars::car_paint_exceptions(db, 251, ex251, &err));
    CHECK(ex251.found && ex251.no_mirrors && ex251.no_paintable_wing);

    // Level-4 bodykit resolution on car 251 (FrontBumper 251001 Lv4 -> bumperfz, RearBumper 251001 Lv4 -> bumperrz).
    cars::FittedParts fit_lv4;
    fit_lv4["List_UpgradeCarBodyFrontBumper"] = 251001;
    fit_lv4["List_UpgradeCarBodyRearBumper"] = 251001;
    cars::VisualSetup vis_lv4;
    CHECK(cars::resolve_visual_setup(db, 251, fit_lv4, vis_lv4, &err));
    CHECK(vis_lv4.front_bumper_level == 4 && vis_lv4.rear_bumper_level == 4);
    CHECK(vis_lv4.has_rally_bodykit);
    CHECK(std::find(vis_lv4.shown_parts.begin(), vis_lv4.shown_parts.end(), "bumperfz") != vis_lv4.shown_parts.end());
    CHECK(std::find(vis_lv4.shown_parts.begin(), vis_lv4.shown_parts.end(), "bumperrz") != vis_lv4.shown_parts.end());
    CHECK(vis_lv4.paint_exceptions.no_mirrors && vis_lv4.paint_exceptions.no_paintable_wing);
  }

  // 5. Phase R2-3A: Real disc carbin bake preserves per-vertex crush weights (damage_flag = 1).
  {
    cars::Database car_db;
    CHECK(cars::load_database(install, car_db, &err));
    const cars::CarRow* r34 = nullptr;
    for (const auto& r : car_db.cars) {
      if (r.id == 445) { r34 = &r; break; }
    }
    auto r34_ref = install.find_car("NIS_SkylineGTRVSpecII_02");
    CHECK(r34 != nullptr && r34_ref.has_value());
    if (r34 && r34_ref) {
      cars::bake::Bake baked;
      CHECK(cars::bake_car(*r34_ref, *r34, baked, &err));
      bool saw_deform_gt0 = false, saw_rigid_zero = false;
      for (const auto& m : baked.models) {
        if (m.role != cars::bake::Role::kLod0) continue;
        for (const auto& p : m.parts) {
          if (p.name == "body") {
            for (const auto& v : p.lod0_vertices) {
              const float cw = cars::vertex_crush_weight(v);
              if (cw > 0.5f) saw_deform_gt0 = true;
              if (cw == 0.0f) saw_rigid_zero = true;
            }
          }
        }
      }
      CHECK(saw_deform_gt0);
      CHECK(saw_rigid_zero);
    }
  }
}

void test_bake_material_classes_blur_rim_and_crush_weights() {
  using cars::bake::MaterialClass;
  using cars::bake::Role;

  // Extended paint techniques.
  CHECK(cars::classify("body_3", false, Role::kLod0) == MaterialClass::kPaint);
  CHECK(cars::classify("body_simple", false, Role::kExterior) == MaterialClass::kPaint);
  CHECK(cars::classify("headlight_paint", false, Role::kLod0) == MaterialClass::kPaint);

  // Spinning blur-rim restoration: NEVER kHidden and NEVER kGlass even when transparent=true.
  CHECK(cars::classify("blur_rim", true, Role::kWheel) == MaterialClass::kRim);
  CHECK(cars::classify("blur_lip", true, Role::kWheel) == MaterialClass::kRim);
  CHECK(cars::classify("chrome_blur_rim", true, Role::kWheel) == MaterialClass::kChrome);
  CHECK(cars::classify("chrome_blur_lip", true, Role::kWheel) == MaterialClass::kChrome);
  CHECK(cars::classify_blur_rim("blur_rim") == cars::BlurRimKind::kBlurRim);
  CHECK(cars::classify_blur_rim("chrome_blur_rim") == cars::BlurRimKind::kChromeBlurRim);
  CHECK(cars::classify_blur_rim("blur_lip") == cars::BlurRimKind::kBlurLip);
  CHECK(cars::classify_blur_rim("chrome_blur_lip") == cars::BlurRimKind::kChromeBlurLip);
  CHECK(cars::is_blur_rim_technique("BLUR_RIM"));
  CHECK(!cars::is_blur_rim_technique("rim"));

  // Rare lamp techniques in bake classifier stay aligned with drive::classify_lamp_technique.
  CHECK(cars::classify("headlightl_lod0", false, Role::kLod0) == MaterialClass::kLamp);
  CHECK(cars::classify("headlightr_lod0", false, Role::kLod0) == MaterialClass::kLamp);
  CHECK(cars::classify("taillightl_lod0", false, Role::kLod0) == MaterialClass::kLamp);
  CHECK(cars::classify("taillightr_lod0", false, Role::kLod0) == MaterialClass::kLamp);

  // Synthetic HEND3N crush-weight decoding test.
  fh1::CarbinPart part{};
  part.bounds = {0, 0, 0, -1.0f, -0.5f, -1.0f, 1.0f, 0.5f, 1.0f};  // |he| = sqrt(2.25) = 1.5 m
  fh1::CarbinDamageBlock blk{};
  blk.kind = 3;
  blk.count = 3;
  blk.stride = 4;
  blk.extra = 1;
  // 4-byte buffer id prefix + 3 HEND3N words:
  // v0: (0, 0, 0) -> 0.0
  // v1: z = +26/511 (~0.0509), disp = 0.0509 * 1.5 = 0.0763 m -> weight ~ 0.509
  // v2: z = -100/511 (~-0.1957), disp = 0.2935 m -> clamped to 1.0
  auto push_be32 = [](std::vector<uint8_t>& dst, uint32_t w) {
    dst.push_back(uint8_t((w >> 24) & 0xFF));
    dst.push_back(uint8_t((w >> 16) & 0xFF));
    dst.push_back(uint8_t((w >> 8) & 0xFF));
    dst.push_back(uint8_t(w & 0xFF));
  };
  push_be32(blk.table, 0x12345678u);  // buffer id
  push_be32(blk.table, 0x00000000u);  // v0
  push_be32(blk.table, (26u & 0x3FFu) << 22);  // v1
  push_be32(blk.words, (uint32_t(-100) & 0x3FFu) << 22);  // v2
  const auto weights = cars::decode_carbin_crush_weights(part, blk, 3);
  CHECK(weights.size() == 3);
  CHECK(weights[0] == 0.0f);
  CHECK(weights[1] > 0.45f && weights[1] < 0.57f);
  CHECK(std::fabs(weights[2] - 1.0f) < 1e-4f);

  cars::bake::Vertex bv{};
  bv.lighting[cars::kCrushWeightLightingChannel] = 128;
  CHECK(std::fabs(cars::vertex_crush_weight(bv) - (128.0f / 255.0f)) < 1e-4f);
}

void test_pt23_pt24_pt25_playtest_fixes() {
  // --- PT-23: Car lamp glow sprite evaluation (no floating red tail blobs while cruising) ---
  {
    fh1::CarbinLampPoint rear_tail_02{};
    rear_tail_02.position = {-0.57f, 0.53f, -1.90f};
    rear_tail_02.direction = {0.0f, 0.0f, -1.0f};
    rear_tail_02.channel_mask = 0x02u;

    fh1::CarbinLampPoint rear_halo_01{};
    rear_halo_01.position = {-0.61f, 0.58f, -2.01f};
    rear_halo_01.direction = {0.0f, 0.0f, -1.0f};
    rear_halo_01.channel_mask = 0x01u;
    rear_halo_01.lamp_kind = 4;

    fh1::CarbinLampPoint brake_04{};
    brake_04.position = {-0.57f, 0.53f, -1.89f};
    brake_04.direction = {0.0f, 0.0f, -1.0f};
    brake_04.channel_mask = 0x04u;

    fh1::CarbinLampPoint front_head_01{};
    front_head_01.position = {-0.50f, 0.35f, 1.89f};
    front_head_01.direction = {0.0f, 0.0f, 1.0f};
    front_head_01.channel_mask = 0x01u;

    // Player car cruising at night (head=1, brake=0): ZERO red flare blobs on both 0x02 and rear 0x01!
    const auto cruise_02 = cars::evaluate_car_lamp_glow_sprite(
        rear_tail_02, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, true, 5.0f, 1.0f);
    const auto cruise_r01_corrado = cars::evaluate_car_lamp_glow_sprite(
        rear_halo_01, 0.45f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, true, 5.0f, 1.0f);
    const auto cruise_r01_viper = cars::evaluate_car_lamp_glow_sprite(
        rear_halo_01, 0.59f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, true, 5.0f, 1.0f);
    const auto cruise_04 = cars::evaluate_car_lamp_glow_sprite(
        brake_04, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, true, 5.0f, 1.0f);
    CHECK(!cruise_02.emit);
    CHECK(!cruise_r01_corrado.emit);
    CHECK(!cruise_r01_viper.emit);
    CHECK(!cruise_04.emit);

    // Braking: dedicated 0x04 emits proportionally with tight housing radius (<= 0.16 m) & seated bias.
    const auto brk_half = cars::evaluate_car_lamp_glow_sprite(
        brake_04, 0.40f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, true, true, 5.0f, 1.0f);
    const auto brk_full = cars::evaluate_car_lamp_glow_sprite(
        brake_04, 0.40f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, true, true, 5.0f, 1.0f);
    CHECK(brk_half.emit && brk_full.emit);
    CHECK(brk_full.intensity > brk_half.intensity * 1.8f);
    CHECK(brk_full.radius_m >= 0.06f && brk_full.radius_m <= 0.16f);
    CHECK(brk_full.view_bias_m <= 0.08f);

    // Viper-style car (no 0x04 points): rear 0x01 lights cleanly as brake glow ONLY while braking.
    const auto viper_brk = cars::evaluate_car_lamp_glow_sprite(
        rear_halo_01, 0.59f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, false, true, 5.0f, 1.0f);
    CHECK(viper_brk.emit && viper_brk.radius_m <= 0.16f && viper_brk.intensity > 2.0f);

    // Front headlamp still emits crisply at night.
    const auto head_ok = cars::evaluate_car_lamp_glow_sprite(
        front_head_01, 0.60f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, true, 5.0f, 1.0f);
    CHECK(head_ok.emit && head_ok.intensity > 4.0f);

    // Nearby AI car cruising has no blob; distant AI car at night gets ultra-subtle pinpoint only.
    const auto ai_near = cars::evaluate_car_lamp_glow_sprite(
        rear_tail_02, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, false, 18.0f, 1.0f);
    const auto ai_far = cars::evaluate_car_lamp_glow_sprite(
        rear_tail_02, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, false, 85.0f, 1.0f);
    CHECK(!ai_near.emit);
    CHECK(ai_far.emit && ai_far.radius_m <= 0.095f && ai_far.intensity < 0.60f);
  }

  // --- PT-25: Active rear spoiler deployment bounds & pitch direction ---
  {
    // Stowed (deploy == 0): exact zero transform.
    const auto stowed = cars::compute_active_wing_deploy(0.0f, 0.075f, 0.0f, false, false, false);
    CHECK(stowed.lift_m == 0.0f && stowed.pitch_rad == 0.0f);

    // Flush / strutless decklid spoiler (VW Corrado VR6): realistic bounded lift (<= 4.2 cm),
    // negative pitch_rad (lifts trailing +Z edge for downforce), hinged near front mount.
    const auto corrado = cars::compute_active_wing_deploy(1.0f, 0.075f, 0.0f, false, false, false);
    CHECK(corrado.lift_m >= 0.020f && corrado.lift_m <= 0.042f);
    CHECK(corrado.pitch_rad < -0.05f && corrado.pitch_rad > -0.18f);
    CHECK(corrado.pivot_z_frac <= 0.25f);

    // Linked pedestal active wing (Veyron / MP4-12C): blade + linkage stay tightly coupled.
    const auto blade = cars::compute_active_wing_deploy(1.0f, 0.09f, 0.15f, true, false, false);
    const auto link = cars::compute_active_wing_deploy(1.0f, 0.09f, 0.15f, true, true, false);
    CHECK(blade.lift_m >= 0.042f && blade.lift_m <= 0.072f);
    CHECK(blade.pitch_rad < -0.10f);
    CHECK(link.lift_m > blade.lift_m * 0.80f && link.lift_m <= blade.lift_m);

    // Independent body aero flaps (Huayra): minimal vertical shift, crisp hinged deflection.
    const auto flap = cars::compute_active_wing_deploy(1.0f, 0.04f, 0.0f, false, false, true);
    CHECK(flap.lift_m <= 0.01f && flap.pitch_rad < -0.20f && flap.pivot_z_frac <= 0.10f);
  }

  // --- PT-24: Driver.zip num_lods == 1 .skinbin layout verification (when disc available) ---
  {
    const fs::path game_root = find_game_root();
    if (!game_root.empty() && fs::exists(game_root / "media" / "cars" / "Driver.zip")) {
      std::string err;
      auto arc = fh1::Archive::open(game_root / "media" / "cars" / "Driver.zip", &err);
      CHECK(arc != nullptr);
      if (arc) {
        auto rd = arc->read("Driver01.skinbin");
        CHECK(rd.ok() && rd.data.size() > 0x2c);
        if (rd.ok() && rd.data.size() > 0x2c) {
          auto be32 = [&](size_t off) -> uint32_t {
            return (uint32_t(rd.data[off]) << 24) | (uint32_t(rd.data[off + 1]) << 16) |
                   (uint32_t(rd.data[off + 2]) << 8) | uint32_t(rd.data[off + 3]);
          };
          const uint32_t num_lods = be32(0x04);
          CHECK(num_lods == 1u);
          const size_t vert_count_off = 0x18 + size_t(num_lods) * 8u;
          const size_t verts_off = 0x20 + size_t(num_lods) * 8u;
          const uint32_t vert_count = be32(vert_count_off);
          const size_t idx_base = verts_off + size_t(vert_count) * 36u;
          CHECK(vert_count > 1000u && idx_base + 4u <= rd.data.size());
          const uint32_t total_idx = be32(idx_base);
          CHECK(total_idx >= 3000u && idx_base + 4u + size_t(total_idx) * 2u <= rd.data.size());
          // Confirm head bone (bone 2) and arm/hand bones (4, 5, 7, 8) are populated.
          uint32_t head_verts = 0, arm_verts = 0;
          for (uint32_t i = 0; i < vert_count; ++i) {
            const uint8_t b0 = rd.data[verts_off + size_t(i) * 36u + 28u];
            if (b0 == 2) ++head_verts;
            if (b0 == 4 || b0 == 5 || b0 == 7 || b0 == 8) ++arm_verts;
          }
          CHECK(head_verts >= 500u);
          CHECK(arm_verts >= 500u);
        }
      }
    }
  }
}

}  // namespace

int main() {
  test_livery_projection_yflip_aspect_mirror_and_mask();
  test_tire_rolling_radius_and_compound_mapping();
  test_bake_material_classes_blur_rim_and_crush_weights();
  test_pt23_pt24_pt25_playtest_fixes();
  test_disc_tire_compound_camber_and_brakes();
  if (g_failed) {
    std::fprintf(stderr, "cars_visual_parity_tests: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("cars_visual_parity_tests: all passed\n");
  return 0;
}
