// Unit tests for Slice 7 Milestone 2: visual upgrade parts, 285 aftermarket rims,
// upgraded brakes (media/brakes/), CarTrackOffsets, ride height & camber visuals,
// and the M1 FJ Cruiser (TOY_FJCruiser_12) per-car tyre fix.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "cars/bake.h"
#include "cars/database.h"
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
  for (const char* p : {R"(C:\Users\neri\code\horizon1-recomp\.local\game\base)",
                        R"(C:\Users\neri\code\shift-engine\.local\game\base)"}) {
    if (fs::exists(fs::path(p) / "media" / "db" / "gamedb.slt")) return p;
  }
  return {};
}

void test_per_car_tyre_rules() {
  CHECK(cars::per_car_tyre_media("TOY_FJCruiser_12") == "tireatoy_fjcruiser_12");
  CHECK(cars::per_car_tyre_media("toy_fjcruiser_12") == "tireatoy_fjcruiser_12");
  CHECK(cars::per_car_tyre_media("FOR_SVTRaptor_12") == "tireafor_svtraptor_12");
  CHECK(cars::per_car_tyre_media("FOR_SVTRaptor_12_LCE") == "tireafor_svtraptor_12");
  CHECK(cars::per_car_tyre_media("FOR_SVTRaptor_12_HALO") == "tireafor_svtraptor_12");
  CHECK(cars::per_car_tyre_media("HUM_H1Alpha_06") == "tireahum_h1alpha_06");
  CHECK(cars::per_car_tyre_media("JEE_Wrangler_12") == "tireajee_wrangler_12");
  CHECK(cars::per_car_tyre_media("JEE_WranglerRubicon_12") == "tireajee_wrangler_12");
  CHECK(cars::per_car_tyre_media("VW_Corrado_95").empty());
  CHECK(cars::is_own_tyre_car("TOY_FJCruiser_12"));
  CHECK(!cars::is_own_tyre_car("NIS_SkylineGTR_02"));

  // Verify FJ Cruiser tyre sizing weights match CARS.md (16" rim, 265/75):
  // wB = (16 - 14) / 10 = 0.2, outer = 0.2032 + 0.265*0.75 = 0.40195 m,
  // wA = (0.40195 - 0.2850) / 0.1301 ~= 0.8989.
  const double rim_r = 16.0 * 0.0254 * 0.5;
  const double outer = rim_r + 0.265 * 0.75;
  const double w_b = (16.0 - 14.0) / 10.0;
  const double w_a = (outer - 0.2850) / 0.1301;
  CHECK(std::fabs(w_b - 0.2) < 1e-6);
  CHECK(std::fabs(outer - 0.40195) < 1e-4);
  CHECK(std::fabs(w_a - 0.8989) < 2e-3);

  CHECK(std::string(cars::brake_type_stem(1)) == "drum");
  CHECK(std::string(cars::brake_type_stem(2)) == "rotor_steel");
  CHECK(std::string(cars::brake_type_stem(3)) == "rotor_drilled");
  CHECK(std::string(cars::brake_type_stem(4)) == "rotor_slotted");
  CHECK(std::string(cars::brake_type_stem(5)) == "rotor_carbon");
}

void test_disc_visual_upgrades_and_rims() {
  const fs::path game_root = find_game_root();
  if (game_root.empty()) {
    std::printf("  (skipping disc visual upgrade checks: base game_root not found)\n");
    return;
  }
  std::string err;
  cars::Install install;
  CHECK(cars::discover_install(game_root, {}, {}, install, &err));
  cars::PartsDb db;
  CHECK(db.open(install, &err));

  // 1. Aftermarket rims (List_Wheels) & on-demand disc wheel loader.
  std::vector<cars::WheelRow> rims;
  CHECK(cars::query_aftermarket_wheels(db, rims, &err));
  std::printf("  aftermarket rims in List_Wheels: %zu\n", rims.size());
  CHECK(rims.size() >= 100);

  // Verify on-demand loading of aftermarket rims from media/wheels/*.zip.
  int loaded_ok = 0;
  for (size_t i = 0; i < std::min<size_t>(rims.size(), 15); ++i) {
    cars::bake::Bake wb;
    if (cars::load_wheel_from_disc(game_root, rims[i].media_name, false, wb, &err) && !wb.models.empty()) {
      ++loaded_ok;
    }
  }
  std::printf("  on-demand disc rim load sample: %d/15\n", loaded_ok);
  CHECK(loaded_ok == 15);

  // Verify per-car tyre on-demand loading (Raptor & Wrangler present on base disc).
  cars::bake::Bake raptor_tyre;
  CHECK(cars::load_wheel_from_disc(game_root, "tireafor_svtraptor_12", true, raptor_tyre, &err));
  CHECK(!raptor_tyre.models.empty());

  // 2. Upgraded brake rotors & calipers from media/brakes/.
  for (int type_id : {2, 3, 4, 5}) {
    cars::bake::Model rotor;
    CHECK(cars::load_brake_model(game_root, 12, type_id, 4, true, rotor, &err));
    CHECK(!rotor.parts.empty());
  }
  for (int pistons : {2, 4, 6, 8}) {
    cars::bake::Model cal;
    CHECK(cars::load_brake_model(game_root, 12, 4, pistons, false, cal, &err));
    CHECK(!cal.parts.empty());
  }

  // 3. CarTrackOffsets loading (car 251 has UpgradeFrontOffset=0.015, car 248 has FrontOffset=-0.01).
  cars::CarTrackOffsetRow off248, off251;
  CHECK(cars::query_track_offsets(db, 248, off248, &err) && off248.found);
  CHECK(std::fabs(off248.front_offset - (-0.01)) < 1e-6);
  CHECK(cars::query_track_offsets(db, 251, off251, &err) && off251.found);
  CHECK(std::fabs(off251.upgrade_front_offset - 0.015) < 1e-6);

  // 4. VisualSetup on Corrado (309): race wing, race front bumper, roll cage,
  //    weight reduction, race springs, race brakes, wider tyres, ATS_Classic (640).
  cars::FittedParts fit;
  fit["List_UpgradeRearWing"] = 309002;                    // WingRace
  fit["List_UpgradeCarBodyFrontBumper"] = 309005;          // BumperFRace
  fit["List_UpgradeCarBodyChassisStiffness"] = 309003;     // CageRace
  fit["List_UpgradeCarBodyWeight"] = 309003;               // Race weight
  fit["List_UpgradeSpringDamper"] = 309003;                // Race springs (lowered)
  fit["List_UpgradeBrakes"] = 309003;                      // Race brakes (13/10, carbon, 6/4 pistons)
  fit["List_UpgradeCarBodyTireWidthFront"] = 309002;       // Wider tyres
  fit["List_UpgradeCarBodyTireWidthRear"] = 309002;
  fit["List_UpgradeRimSizeFront"] = 309001;
  fit["List_UpgradeRimSizeRear"] = 309001;
  fit["List_Wheels"] = 640;                                // ATS_Classic aftermarket rim

  cars::VisualSetup vis_stock, vis_upg;
  CHECK(cars::resolve_visual_setup(db, 309, {}, vis_stock, &err));
  CHECK(cars::resolve_visual_setup(db, 309, fit, vis_upg, &err, -1.0f, -1.0f, -2.5f, -1.5f));
  CHECK(vis_upg.active);
  std::set<std::string> shown(vis_upg.shown_parts.begin(), vis_upg.shown_parts.end());
  CHECK(shown.count("wingrace") == 1);
  CHECK(shown.count("bumperfrace") == 1);
  CHECK(shown.count("cagerace") == 1);
  CHECK(vis_upg.wheel_media == "ats_classic");
  CHECK(vis_upg.tyre_width_mm[0] > vis_stock.tyre_width_mm[0]);
  CHECK(vis_upg.rim_inches[0] > vis_stock.rim_inches[0]);
  CHECK(vis_upg.ride_height_m[0] < vis_stock.ride_height_m[0]);  // lowered!
  CHECK(vis_upg.brakes[0].level == 3 && vis_upg.brakes[0].size_in == 13 && vis_upg.brakes[0].pistons == 6);
  CHECK(vis_upg.brakes[0].type_id == 5);
  CHECK(vis_upg.camber_rad[0] < -0.04f && vis_upg.camber_rad[1] < -0.02f);
  std::printf("  Corrado 309 visuals: rim=%s width=%lld->%lldmm rh=%.3f->%.3fm brakes=%d\"/%dp\n",
              vis_upg.wheel_media.c_str(), (long long)vis_stock.tyre_width_mm[0], (long long)vis_upg.tyre_width_mm[0],
              vis_stock.ride_height_m[0], vis_upg.ride_height_m[0], vis_upg.brakes[0].size_in,
              vis_upg.brakes[0].pistons);
}

}  // namespace

int main() {
  test_per_car_tyre_rules();
  test_disc_visual_upgrades_and_rims();
  if (g_failed) {
    std::fprintf(stderr, "visual_upgrades_tests: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("visual_upgrades_tests: all passed\n");
  return 0;
}
