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
}

}  // namespace

int main() {
  test_livery_projection_yflip_aspect_mirror_and_mask();
  test_tire_rolling_radius_and_compound_mapping();
  test_disc_tire_compound_camber_and_brakes();
  if (g_failed) {
    std::fprintf(stderr, "cars_visual_parity_tests: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("cars_visual_parity_tests: all passed\n");
  return 0;
}
