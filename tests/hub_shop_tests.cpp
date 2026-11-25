// Unit tests for Slice 7 Milestone 3: 3D Festival Hub screens (uiautoshow Make
// Carousel + 2-car tutorial + factory colour picker, uiworkshop / Dak's Garage
// 8-category UpgradeShop with Conversions/Auto-Upgrade/35-slider Tuning/50%
// Sell Part & live PI/5-stat diffs, GarageBrowser sort/filter/Sell Car, 3D hub
// studio plinth + drop-shadow stage, and Barn Find restoration card HUD).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "autoshow.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "game_hud.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "hub_scene.h"
#include "ui.h"
#include "ui4_chrome.h"
#include "upgrade_shop.h"

// Include LiveryEditor implementation directly in hub_shop_tests (hub_shop_tests links all
// dependencies — shift_cars, carview_render, carview_hud — without linking livery_editor.cpp).
#include "../apps/carview/livery_editor.cpp"

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
  if (const char* env = std::getenv("SHIFT_GAME_ROOT"); env && *env) {
    if (fs::exists(fs::path(env) / "media" / "db" / "gamedb.slt")) return env;
  }
  for (const char* p : {R"(../horizon1-recomp/.local\game\base)",
                        R"(.local\game\base)"}) {
    if (fs::exists(fs::path(p) / "media" / "db" / "gamedb.slt")) return p;
  }
  return {};
}

void test_hub_stage_and_camera() {
  for (auto room : {carview::HubRoom::kAutoshow, carview::HubRoom::kWorkshop, carview::HubRoom::kPaintShop,
                    carview::HubRoom::kGarage, carview::HubRoom::kBarnFind}) {
    carview::WorldTriangles stage = carview::build_hub_stage(room, 0.95f, 2.2f, 0.0f);
    CHECK(!stage.positions.empty());
    CHECK(stage.positions.size() == stage.group.size() * 3);
    CHECK(stage.group.size() >= 200);
    CHECK(!stage.colours.empty());

    carview::HubCamera cam;
    cam.set_focus(carview::HubCameraFocus::kTurntable);
    cam.step(0.5f);
    carview::ShowroomFrame frame;
    carview::build_hub_frame(room, cam, 1280, 720, frame, false);
    CHECK(frame.drive[3] == 0.0f);  // studio cubemap mode
    CHECK(frame.options[0] >= 0.35f && frame.options[0] <= 0.70f);
  }

  CHECK(carview::focus_for_part_enum(43) == carview::HubCameraFocus::kWheels);
  CHECK(carview::focus_for_part_enum(9) == carview::HubCameraFocus::kRear);
  CHECK(carview::focus_for_part_enum(34) == carview::HubCameraFocus::kFront);
  CHECK(carview::focus_for_part_enum(36) == carview::HubCameraFocus::kHood);

  // Barn Find Restoration Card UI rasterization.
  carview::BarnFindCard card;
  card.visible = true;
  card.barn_name = "BARN FIND #1";
  card.car_name = "1992 BUGATTI EB110 SS";
  card.year = 1992;
  card.restore_progress = 1.0f;
  carview::ui::List list;
  carview::draw_barn_find_hud(list, 1280.0f, 720.0f, card);
  CHECK(!list.empty());
  std::vector<uint8_t> rgba(1280u * 720u * 4u, 0);
  carview::ui::rasterize(list.items(), rgba, 1280, 720);
  int lit = 0;
  for (size_t i = 3; i < rgba.size(); i += 4)
    if (rgba[i] > 0) ++lit;
  CHECK(lit > 50000);
}

void test_livery_editor_groups_undo_and_catalog() {
  // 1. OEM #1..N 1-based swatch label fix (no off-by-one 0..N-1).
  CHECK(carview::oem_swatch_label(0) == "OEM #1");
  CHECK(carview::oem_swatch_label(13) == "OEM #14");

  carview::LiveryEditor ed;
  std::string msg;
  CHECK(ed.open(nullptr, {}, 309, {}, msg));
  CHECK(!ed.can_undo());

  // 2. Vinyl Group multi-layer transforms: moving, scaling, rotating, recoloring, flipping,
  //    skewing, adjusting alpha, and duplicating all affect every layer sharing group_id!
  ed.set_tab(carview::EditorTab::kVinylLayers);
  ed.add_vinyl_layer("builtin:square", 0xFF0000u);
  ed.transform_layer(-0.10f, 0.0f, 0.0f, 0.0f, 0.0f);  // layer 0 at x=0.40, y=0.50
  ed.add_vinyl_layer("builtin:circle", 0x00FF00u);
  ed.transform_layer(+0.10f, 0.0f, 0.0f, 0.0f, 0.0f);  // layer 1 at x=0.60, y=0.50
  CHECK(ed.document().layers.size() == 2);

  // Assign both layers to group 1.
  ed.select_layer_index(0);
  ed.toggle_layer_group();
  ed.select_layer_index(1);
  ed.toggle_layer_group();
  CHECK(ed.document().layers[0].group_id == 1 && ed.document().layers[1].group_id == 1);

  const float x0_before = ed.document().layers[0].x;
  const float x1_before = ed.document().layers[1].x;
  const float sx0_before = ed.document().layers[0].scale_x;
  const float sx1_before = ed.document().layers[1].scale_x;
  const size_t undo_before_move = ed.undo_depth();

  // Translate group together.
  ed.transform_layer(0.05f, -0.04f, 0.0f, 0.0f, 0.0f);
  CHECK(std::fabs(ed.document().layers[0].x - (x0_before + 0.05f)) < 1e-4f);
  CHECK(std::fabs(ed.document().layers[1].x - (x1_before + 0.05f)) < 1e-4f);
  CHECK(std::fabs(ed.document().layers[0].y - 0.46f) < 1e-4f);
  CHECK(std::fabs(ed.document().layers[1].y - 0.46f) < 1e-4f);
  CHECK(ed.undo_depth() == undo_before_move + 1);

  // Scale group together.
  ed.transform_layer(0.0f, 0.0f, 0.06f, 0.04f, 0.0f);
  CHECK(std::fabs(ed.document().layers[0].scale_x - (sx0_before + 0.06f)) < 1e-4f);
  CHECK(std::fabs(ed.document().layers[1].scale_x - (sx1_before + 0.06f)) < 1e-4f);

  // Rotate group 90 degrees around group centroid -> both layers orbit centroid.
  const float gx0 = ed.document().layers[0].x;
  const float gx1 = ed.document().layers[1].x;
  const float cx = 0.5f * (gx0 + gx1);
  ed.transform_layer(0.0f, 0.0f, 0.0f, 0.0f, 90.0f);
  CHECK(std::fabs(ed.document().layers[0].x - cx) < 1e-3f);
  CHECK(std::fabs(ed.document().layers[1].x - cx) < 1e-3f);
  CHECK(std::fabs(ed.document().layers[0].rot_deg - 90.0f) < 1e-3f);
  CHECK(std::fabs(ed.document().layers[1].rot_deg - 90.0f) < 1e-3f);

  // Recolor, flip, skew, alpha act on entire group.
  const uint32_t col0_before = ed.document().layers[0].rgb;
  const uint32_t col1_before = ed.document().layers[1].rgb;
  ed.recolor_layer_hsv(0.25f, 0.0f, 0.0f);
  CHECK(ed.document().layers[0].rgb != col0_before && ed.document().layers[1].rgb != col1_before);
  ed.toggle_layer_flip(true);
  CHECK(ed.document().layers[0].flip_x && ed.document().layers[1].flip_x);
  ed.adjust_layer_skew(0.20f);
  CHECK(std::fabs(ed.document().layers[0].skew - 0.20f) < 1e-4f &&
        std::fabs(ed.document().layers[1].skew - 0.20f) < 1e-4f);
  ed.adjust_layer_alpha(-0.25f);
  CHECK(std::fabs(ed.document().layers[0].alpha - 0.75f) < 1e-4f &&
        std::fabs(ed.document().layers[1].alpha - 0.75f) < 1e-4f);

  // Duplicate group creates a full copy of all group members under a fresh group_id.
  CHECK(ed.duplicate_selected_layer());
  CHECK(ed.document().layers.size() == 4);
  CHECK(ed.document().layers[2].group_id > 1 &&
        ed.document().layers[2].group_id == ed.document().layers[3].group_id);

  // 3. Undo stack restores prior states cleanly.
  CHECK(ed.undo());
  CHECK(ed.document().layers.size() == 2);
  CHECK(ed.undo());  // undo alpha
  CHECK(std::fabs(ed.document().layers[0].alpha - 1.0f) < 1e-4f);

  // 4. Reset to factory paint clears custom paint & vinyl layers (and can be undone).
  ed.set_tab(carview::EditorTab::kPaint);
  ed.step_hsv(0.3f, 0.0f, 0.0f);
  ed.reset_to_factory_paint();
  CHECK(ed.document().layers.empty());
  CHECK(!ed.document().paint.hood.active && !ed.document().paint.wing.active && !ed.document().paint.rims.active);
  CHECK(ed.undo());  // undo factory reset restores layers!
  CHECK(ed.document().layers.size() == 2);

  // 5. Design Catalog save / scan / load round-trip in temp liveries/ directory.
  const fs::path cat_dir = fs::temp_directory_path() / "shift_hub_shop_livery_catalog_test";
  std::error_code ec;
  fs::remove_all(cat_dir, ec);
  std::string cerr;
  CHECK(ed.save_to_catalog(cat_dir, "Track Stripe", &cerr));
  ed.scan_design_catalog(cat_dir);
  CHECK(ed.design_catalog().size() == 1);
  CHECK(ed.design_catalog()[0].layer_count == 2);
  ed.reset_to_factory_paint();
  CHECK(ed.document().layers.empty());
  CHECK(ed.load_selected_design(&cerr));
  CHECK(ed.document().layers.size() == 2);
  fs::remove_all(cat_dir, ec);

  // 6. Debounced / throttled GPU preview upload eliminates per-tick vkDeviceWaitIdle stalls.
  ed.set_preview_throttle_seconds(0.12);
  ed.mark_vinyl_preview_uploaded(10.00);
  CHECK(!ed.has_pending_vinyl_preview() && !ed.needs_vinyl_preview_upload(10.05));
  ed.transform_layer_continuous(0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 10.02);
  CHECK(ed.has_pending_vinyl_preview());
  CHECK(!ed.needs_vinyl_preview_upload(10.05));  // throttled inside 120 ms window!
  CHECK(ed.needs_vinyl_preview_upload(10.15));   // allowed after window elapses!
  ed.mark_vinyl_preview_uploaded(10.15);
  CHECK(!ed.has_pending_vinyl_preview());
}

void test_disc_hub_shops_and_garage() {
  const fs::path game_root = find_game_root();
  if (game_root.empty()) {
    std::printf("  (skipping disc hub/shop tests: base game_root not found)\n");
    return;
  }
  std::string err;
  cars::Install install;
  CHECK(cars::discover_install(game_root, {}, {}, install, &err));
  cars::PartsDb db;
  CHECK(db.open(install, &err));
  cars::PhysicsSettings phys_settings;
  CHECK(cars::load_physics_settings(install.media / "physics.zip", phys_settings, &err));
  garage::PiConfig pi_cfg;
  CHECK(garage::load_pi_config(install.media / "physics.zip", pi_cfg, &err));
  std::vector<garage::ClassBand> bands;
  CHECK(garage::load_class_bands(game_root, bands, &err));

  // 1. Autoshow Make Carousel entry mode + "All Affordable" + "All Cars" + Sort/Filter + >14 swatch scroll + [OWNED] + Test Drive.
  carview::Autoshow autoshow;
  CHECK(autoshow.open(db, {}, err, 0, false));
  CHECK(autoshow.step() == carview::AutoshowStep::kMakeSelect);
  CHECK(!autoshow.makes().empty());
  CHECK(autoshow.makes()[0].name == "All Affordable");
  CHECK(autoshow.visible_cars().empty());
  CHECK(autoshow.empty_reason() == "no affordable cars");

  // 115's strip: the makes alone (All Affordable and All Cars are off it);
  // Y lists the affordable cars, and the strip comes back on the make it left.
  {
    carview::Autoshow strip_show;
    CHECK(strip_show.open(db, {}, err, 500000, false));
    const std::vector<size_t> strip = strip_show.strip_makes();
    CHECK(strip.size() > 25);
    for (size_t i : strip) CHECK(strip_show.makes()[i].make_id >= 0 || strip_show.makes()[i].make_id == -3);
    strip_show.move_strip_make(0, 500000);
    CHECK(strip_show.selected_make_index() == strip[0]);
    strip_show.move_strip_make(-1, 500000);
    CHECK(strip_show.selected_make_index() == strip.back());
    strip_show.move_strip_make(2, 500000);
    const size_t left = strip_show.selected_make_index();
    CHECK(left == strip[1]);
    strip_show.select_affordable(500000);
    CHECK(strip_show.makes()[strip_show.selected_make_index()].make_id == -1);
    strip_show.move_strip_make(0, 500000);
    CHECK(strip_show.selected_make_index() == left);
  }

  // Move to "All Cars" / real makes and verify Sort/Filter/Test Drive/Swatch scrolling.
  CHECK(autoshow.makes().size() > 25);
  autoshow.move_make(1, 500000);
  CHECK(!autoshow.visible_cars().empty());
  const carview::AutoshowCar* first_car = autoshow.selected_car();
  CHECK(first_car != nullptr);
  if (first_car && first_car->swatches.size() > 1) {
    const int64_t seq0 = autoshow.selected_colour_sequence();
    autoshow.move_colour(1);
    const int64_t seq1 = autoshow.selected_colour_sequence();
    CHECK(seq0 != seq1);
  }
  // Toggle All Cars, sort by Price/Year/PI/Name, filter by Class/Drivetrain.
  autoshow.toggle_all_cars(500000);
  CHECK(autoshow.all_cars_mode() && !autoshow.visible_cars().empty());
  autoshow.cycle_sort(1);
  CHECK(autoshow.sort_mode() == carview::AutoshowSort::kPrice);
  if (autoshow.visible_cars().size() >= 2) {
    CHECK(autoshow.visible_cars()[0].dealer.base_cost <= autoshow.visible_cars()[1].dealer.base_cost);
  }
  autoshow.cycle_sort(1);
  CHECK(autoshow.sort_mode() == carview::AutoshowSort::kYear);
  autoshow.cycle_sort(1);
  CHECK(autoshow.sort_mode() == carview::AutoshowSort::kPi);
  autoshow.cycle_sort(1);
  CHECK(autoshow.sort_mode() == carview::AutoshowSort::kName);
  autoshow.set_sort(carview::AutoshowSort::kDefault);
  autoshow.cycle_drivetrain_filter(1);
  CHECK(autoshow.drivetrain_filter() == 1);
  for (const auto& vc : autoshow.visible_cars()) CHECK(vc.drive_type_id == 1);
  autoshow.set_drivetrain_filter(0);
  // Check swatch_window scrolling on a car with >14 factory swatches (no 14-swatch truncation!).
  for (size_t ci = 0; ci < autoshow.visible_cars().size(); ++ci) {
    if (autoshow.visible_cars()[ci].swatches.size() > 14) {
      autoshow.select_index(ci);
      const size_t total_sw = autoshow.selected_car()->swatches.size();
      autoshow.select_colour_index(total_sw - 1);
      size_t first_sw = 0, vis_sw = 0;
      autoshow.swatch_window(14, first_sw, vis_sw);
      CHECK(vis_sw == 14 && first_sw > 0 && first_sw + vis_sw == total_sw);
      break;
    }
  }
  // Test Drive preparation hook.
  std::string td_media;
  int64_t td_cid = 0, td_col = -1;
  CHECK(autoshow.prepare_test_drive(td_media, td_cid, td_col) && !td_media.empty() && td_cid > 0);

  // 2. Autoshow 2-car tutorial first-purchase flow (cars 1323 & 1009).
  carview::Autoshow tut;
  CHECK(tut.open(db, {}, err, 50000, true));
  CHECK(tut.tutorial_mode());
  CHECK(tut.visible_cars().size() == 2);
  std::set<int64_t> tut_ids;
  for (const auto& c : tut.visible_cars()) tut_ids.insert(c.dealer.id);
  CHECK(tut_ids.count(1323) == 1 && tut_ids.count(1009) == 1);

  // Buy a car from Autoshow into Garage with chosen factory colour.
  garage::Garage g;
  int64_t credits = 500000;
  std::string bought_media, msg;
  tut.move_colour(1);
  const int64_t expected_col = tut.selected_colour_sequence();
  CHECK(tut.buy(db, g, credits, bought_media, msg));
  CHECK(g.cars.size() == 1);
  CHECK(g.selected_car()->colour == int(expected_col));

  // Also add Corrado (309) and R34 (445) so GarageBrowser sort/filter/sell can be tested thoroughly.
  std::map<std::string, int64_t> s309, s445;
  CHECK(db.stock_slots(309, s309, &err));
  CHECK(db.stock_slots(445, s445, &err));
  int64_t gid309 = 0, gid445 = 0;
  CHECK(garage::buy_car(g, 309, 10000, 0.524381, 2, 1, s309, credits, &gid309));
  CHECK(garage::buy_car(g, 445, 50000, 0.62982, 4, 1, s445, credits, &gid445));
  g.selected = gid309;
  // Autoshow [OWNED] badge check against live CareerGarage.
  autoshow.set_owned_cars(g);
  CHECK(autoshow.is_owned(309) && autoshow.is_owned(445));

  // 3. UpgradeShop: all 8 categories, aftermarket rims, conversions, Auto Upgrade, 35-slider Tuning, Sell Part.
  carview::UpgradeShop shop;
  CHECK(shop.open(db, g, gid309, err, &phys_settings, &pi_cfg, &bands));
  CHECK(shop.has_preview_pi());
  CHECK(shop.current_pi().shown_pi == 282);
  // Signs-only discount override prevents Affinity stacking on upgrade prices.
  shop.set_signs_discount_percent(15);
  CHECK(shop.effective_discount_percent(65) == 15);
  shop.set_signs_discount_percent(0);

  // Check Tires & Rims contains aftermarket rims (part_enum 43).
  shop.set_category(carview::ShopCategory::kTiresRims);
  const auto tire_rows = shop.visible_rows(0);
  bool has_rims = false;
  for (const auto& r : tire_rows)
    if (r.part_enum == 43) has_rims = true;
  CHECK(has_rims);

  // Check Conversions contains engine swaps (part_enum 0) with real donor engine names (no "Engine Swap #1"!).
  shop.set_category(carview::ShopCategory::kConversions);
  const auto conv_rows = shop.visible_rows(0);
  CHECK(!conv_rows.empty());
  shop.move_part(1);
  const auto conv_swap_rows = shop.visible_rows(0);
  bool saw_real_engine_swap = false;
  for (const auto& r : conv_swap_rows) {
    CHECK(r.part_label.find("Engine Swap #") == std::string::npos);
    CHECK(r.part_label.find("Drivetrain Swap #") == std::string::npos);
    CHECK(r.part_label.find("Body Conversion #") == std::string::npos);
    if (r.part_enum == 0 && !r.is_stock) {
      saw_real_engine_swap = (r.part_label.find("Scirocco") != std::string::npos ||
                              r.part_label.find("I4T") != std::string::npos);
    }
  }
  CHECK(saw_real_engine_swap);
  shop.move_part(-1);

  // Check ALL performance/handling/drivetrain/tires/aero categories have zero "(L1)"/"(L2)"/"(L3)" placeholders.
  for (auto cat : {carview::ShopCategory::kEngine, carview::ShopCategory::kPlatform,
                   carview::ShopCategory::kDrivetrain, carview::ShopCategory::kTiresRims,
                   carview::ShopCategory::kAero}) {
    shop.set_category(cat);
    const size_t n_slots = shop.visible_rows(0).size();
    for (size_t si = 0; si < n_slots; ++si) {
      shop.select_row(si);
      shop.move_part(1);
      const auto rows_now = shop.visible_rows(0);
      if (si < rows_now.size()) {
        const auto& pl = rows_now[si].part_label;
        CHECK(!pl.empty());
        CHECK(pl.find("(L1)") == std::string::npos);
        CHECK(pl.find("(L2)") == std::string::npos);
        CHECK(pl.find("(L3)") == std::string::npos);
        CHECK(pl.find("_&") == std::string::npos);
        if (rows_now[si].part_enum == 38 || rows_now[si].part_enum == 39) {
          CHECK(pl.find("mm") != std::string::npos);
        }
        if (rows_now[si].part_enum == 10 || rows_now[si].part_enum == 11) {
          CHECK(pl.find("in") != std::string::npos);
        }
      }
      shop.move_part(-1);
    }
  }

  // Fit a non-stock engine upgrade, check PI increases, then refit stock and Sell Part (50%).
  shop.set_category(carview::ShopCategory::kEngine);
  const int pi_before = shop.current_pi().shown_pi;
  shop.move_part(1);
  CHECK(shop.preview_pi().shown_pi >= pi_before);
  const int64_t cr_before = credits;
  CHECK(shop.fit(0, credits, msg));
  CHECK(credits < cr_before);
  CHECK(shop.changed());
  // Refit stock so the bought upgrade can be sold for 50%.
  shop.move_part(-1);
  CHECK(shop.fit(0, credits, msg));
  shop.move_part(1);
  const int64_t cr_pre_sell = credits;
  CHECK(shop.sell_shown_part(credits, msg));
  CHECK(credits > cr_pre_sell);

  // Check Auto Upgrade tab -> E..R1 + Class X (10 rows), fit A class.
  shop.set_category(carview::ShopCategory::kAutoUpgrade);
  const auto auto_rows = shop.visible_rows(0);
  CHECK(auto_rows.size() == 10);
  CHECK(auto_rows.back().label.find("X") != std::string::npos);
  while (shop.selected_row() < 4) shop.move_slot(1);  // index 4 = class 5 (A)
  CHECK(shop.auto_plan().ok && shop.auto_plan().result.class_id == 5);
  CHECK(shop.auto_plan().result.raw_pi <= bands[5].raw_max);
  CHECK(shop.fit(0, credits, msg));
  CHECK(g.find(gid309)->class_id == 5);

  // Check Tuning tab (25..35 sliders depending on drivetrain/gear count) + adjust tire pressure + Race unlock hints.
  shop.set_category(carview::ShopCategory::kTuning);
  CHECK(shop.tuning_sliders().size() >= 24 && shop.tuning_sliders().size() <= 35);
  const auto tune_rows = shop.visible_rows(0);
  bool saw_locked_hint = false;
  for (const auto& tr : tune_rows) {
    if (!tr.fitted) {
      CHECK(tr.state.rfind("LOCKED", 0) == 0);
      CHECK(!tr.unlock_hint.empty());
      saw_locked_hint = true;
    }
  }
  shop.move_part(1);
  CHECK(g.find(gid309)->tune.front_tire_pressure_psi > 0.0);
  (void)saw_locked_hint;

  // 4. GarageBrowser sort (Class/Make/Value/Year), filters (F..X + Favorites), 5 stats + real specs,
  //    prize-car 100 CR sell value & purchased 50% sell value.
  int64_t prize_gid = 0;
  CHECK(garage::award_prize_car(g, 445, 0.62982, 4, 1, s445, &prize_gid));
  carview::GarageBrowser gb;
  gb.refresh(g, &db, bands);
  CHECK(gb.visible().size() == 4);
  // Real specs & 5 stat bars populated on GarageEntry.
  CHECK(gb.selected_entry() != nullptr);
  CHECK(gb.selected_entry()->speed_rating > 0.0 && gb.selected_entry()->braking_rating > 0.0);
  CHECK(!gb.selected_entry()->aspiration.empty());
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kMake);
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kValue);
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kDrivetrain);
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kPi);
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kYear);
  // Back to the original order (class, then PI).
  gb.set_sort(carview::GarageSort::kClass);
  for (size_t i = 1; i < gb.visible().size(); ++i) {
    const auto& a = gb.visible()[i - 1];
    const auto& b = gb.visible()[i];
    CHECK(a.class_id < b.class_id || (a.class_id == b.class_id && a.raw_pi <= b.raw_pi));
  }
  // Class F (0) & Class X (10) filters supported cleanly.
  gb.set_class_filter(0);
  CHECK(gb.class_filter() == 0);
  gb.set_class_filter(10);
  CHECK(gb.class_filter() == 10);
  gb.set_class_filter(-1);
  // Favorites toggle & filter.
  CHECK(gb.toggle_favorite_selected(g, &db, bands));
  gb.toggle_favorites_filter();
  CHECK(gb.favorites_only() && gb.visible().size() == 1 && gb.visible()[0].favorite);
  gb.toggle_favorites_filter();
  CHECK(!gb.favorites_only() && gb.visible().size() == 4);

  // Sell the prize car -> pays exactly 100 CR!
  for (size_t i = 0; i < gb.visible().size(); ++i) {
    if (gb.visible()[i].garage_id == prize_gid) {
      gb.select_index(i);
      break;
    }
  }
  CHECK(gb.selected_entry() && gb.selected_entry()->is_prize && gb.selected_entry()->sell_cr == 100);
  const int64_t cr_before_prize_sell = credits;
  CHECK(gb.sell_selected(g, &db, bands, credits, msg));
  CHECK(credits == cr_before_prize_sell + 100);
  CHECK(g.cars.size() == 3);

  // Select a non-active purchased car and sell it for 50%.
  while (gb.selected_entry() && gb.selected_entry()->is_current) gb.move(1);
  const int64_t cr_before_sell_car = credits;
  CHECK(gb.sell_selected(g, &db, bands, credits, msg));
  CHECK(credits > cr_before_sell_car);
  CHECK(g.cars.size() == 2);

  // R3-CAM-09 & R3-CAM-20: Dak's Garage Barn Find Restoration Bay tab ("READY TO CLAIM")
  // and dedicated "REMOVE CAR" two-step confirmation respecting `is_prize` (0 CR).
  {
    int64_t prize_rm_gid = 0;
    CHECK(garage::award_prize_car(g, 445, 0.62982, 4, 1, s445, &prize_rm_gid));
    gb.refresh(g, &db, bands);
    // Select the newly added non-active prize car.
    for (size_t i = 0; i < gb.visible().size(); ++i) {
      if (gb.visible()[i].garage_id == prize_rm_gid) {
        gb.select_index(i);
        break;
      }
    }
    CHECK(gb.selected_entry() && gb.selected_entry()->is_prize && !gb.selected_entry()->is_current);
    const int64_t cr_before_remove = credits;
    // Step 1 arms confirmation without removing yet.
    CHECK(!gb.remove_selected(g, &db, bands, credits, msg));
    CHECK(gb.remove_confirm_garage_id() == prize_rm_gid);
    // Render HUD while confirm modal is active.
    carview::ui::List l_confirm;
    carview::draw_garage_hud(l_confirm, 1280.0f, 720.0f, gb, bands, credits);
    CHECK(!l_confirm.empty());
    // Step 2 confirms removal -> prize car removed for 0 CR!
    CHECK(gb.remove_selected(g, &db, bands, credits, msg));
    CHECK(credits == cr_before_remove);
    CHECK(g.find(prize_rm_gid) == nullptr);

    // Active car cannot be removed without allow_switch_active.
    for (size_t i = 0; i < gb.visible().size(); ++i) {
      if (gb.visible()[i].is_current) {
        gb.select_index(i);
        break;
      }
    }
    CHECK(!gb.remove_selected(g, &db, bands, credits, msg, /*confirmed=*/true, /*allow_switch_active=*/false));

    // Barn Find Restoration Bay tab: in-progress cannot be claimed, ready-to-claim awards car with is_prize=true!
    std::vector<carview::BarnRestorationEntry> bay = {
        {"barnfind_01", "1956 Jaguar D-Type", 309, 45.0, 20.0, false},
        {"barnfind_02", "1964 Aston Martin DB5", 445, 45.0, 45.0, true},
    };
    gb.set_restorations(bay);
    CHECK(gb.ready_restoration_count() == 1);
    gb.set_tab(carview::GarageTab::kRestorationBay);
    CHECK(gb.active_tab() == carview::GarageTab::kRestorationBay);
    carview::ui::List l_bay;
    carview::draw_garage_hud(l_bay, 1280.0f, 720.0f, gb, bands, credits);
    CHECK(!l_bay.empty());

    std::string claimed_id;
    // Index 0 is still restoring -> claim must fail cleanly.
    CHECK(!gb.claim_selected_restoration(g, &db, bands, claimed_id, msg));
    CHECK(claimed_id.empty() && gb.restorations().size() == 2);
    // Move to index 1 (ready_to_claim = true) -> claim succeeds and awards car with is_prize = true!
    gb.move(1);
    const size_t cars_before_claim = g.cars.size();
    CHECK(gb.claim_selected_restoration(g, &db, bands, claimed_id, msg));
    CHECK(claimed_id == "barnfind_02");
    CHECK(gb.restorations().size() == 1);
    CHECK(g.cars.size() == cars_before_claim + 1);
    CHECK(g.cars.back().car_id == 445 && g.cars.back().is_prize);
    gb.set_tab(carview::GarageTab::kMyCars);
  }

  // 5. Render Autoshow (all 3 steps: 115 Make -> 108 Car -> 114 Color), UpgradeShop (+Tuning), and Garage.
  std::set<std::string> requested_textures;
  carview::ui4::set_texture_loader([&](const std::string& path, std::array<float, 4>& uv) {
    requested_textures.insert(path);
    uv = {0.0f, 0.0f, 0.1f, 0.1f};
    return true;
  });

  autoshow.set_step(carview::AutoshowStep::kMakeSelect);
  CHECK(autoshow.step() == carview::AutoshowStep::kMakeSelect);
  carview::ui::List l_make, l_car, l_col;
  carview::draw_autoshow_hud(l_make, 1280.0f, 720.0f, autoshow, bands, credits);
  CHECK(!autoshow.advance_step());
  CHECK(autoshow.step() == carview::AutoshowStep::kCarSelect);
  carview::draw_autoshow_hud(l_car, 1280.0f, 720.0f, autoshow, bands, credits);
  CHECK(!autoshow.advance_step());
  CHECK(autoshow.step() == carview::AutoshowStep::kColorSelect);
  carview::draw_autoshow_hud(l_col, 1280.0f, 720.0f, autoshow, bands, credits);
  CHECK(autoshow.advance_step());  // ready to buy
  CHECK(autoshow.back_step() && autoshow.step() == carview::AutoshowStep::kCarSelect);

  carview::ui::List l2_parts, l2_tune, l3;
  shop.set_category(carview::ShopCategory::kEngine);
  carview::draw_upgrade_shop_hud(l2_parts, 1280.0f, 720.0f, shop, bands, 20, credits, "VW Corrado VR6");
  shop.set_category(carview::ShopCategory::kTuning);
  carview::draw_upgrade_shop_hud(l2_tune, 1280.0f, 720.0f, shop, bands, 20, credits, "VW Corrado VR6");
  carview::draw_garage_hud(l3, 1280.0f, 720.0f, gb, bands, credits);

  // Verify Phase 3 authentic texture lookups across Autoshow, Upgrades, Tuning, and Garage.
  auto has_prefix = [&](const std::string& pfx) {
    for (const auto& t : requested_textures)
      if (t.rfind(pfx, 0) == 0) return true;
    return false;
  };
  CHECK(has_prefix("T:UI4/Upgrades/"));
  CHECK(has_prefix("T:UI4/Tuning/"));
  CHECK(requested_textures.count("DIRTTEXTURES/STATBARFILL") > 0);
  CHECK(requested_textures.count("DIRTTEXTURES/STATBAREMPTY") > 0);
  CHECK(requested_textures.count("OPTIONS/OPT_BAR_ARROW") > 0);
  CHECK(requested_textures.count("COMMON/SCROLLINDICATOR") > 0);

  // Also verify EventTickets/* and SponsorLogoTile/* helpers resolve real disc paths.
  carview::ui::List l_extra;
  CHECK(carview::ui4::event_ticket_art(l_extra, 0, 0, 200, 120, 50, 2));
  CHECK(carview::ui4::event_ticket_art(l_extra, 0, 0, 200, 120, 0, 10));
  CHECK(carview::ui4::sponsor_logo_tile(l_extra, 0, 0, 128, 64, "Oakley"));
  CHECK(carview::ui4::sponsor_logo_tile(l_extra, 0, 0, 128, 64, "RedBull"));
  CHECK(has_prefix("EVENTTICKETS/STREETRACES/"));
  CHECK(has_prefix("EVENTTICKETS/MISSIONS/"));
  CHECK(requested_textures.count("SPONSORLOGOTILE/OAKLEY") > 0);
  CHECK(requested_textures.count("SPONSORLOGOTILE/REDBULL") > 0);

  carview::ui4::set_texture_loader({});
  CHECK(!l_make.empty() && !l_car.empty() && !l_col.empty() && !l2_parts.empty() && !l2_tune.empty() && !l3.empty());
  std::printf("  hub UI instances: make=%zu car=%zu color=%zu workshop=%zu tuning=%zu garage=%zu\n",
              l_make.items().size(), l_car.items().size(), l_col.items().size(), l2_parts.items().size(),
              l2_tune.items().size(), l3.items().size());

  // 6. PT-43 Autoshow UX/UI parity checks: non-clipping rendered hit zones & UI instances,
  //    live spec comparison deltas, manufacturer/model/paint+finish/caliper navigation,
  //    and affordable/unaffordable purchase guards.
  {
    // Verify emitted UI hit zones and draw rects stay strictly outside hero_car_viewport across all 3 steps!
    auto check_non_clipping_draw = [&](carview::AutoshowStep st, bool show_specs) {
      autoshow.set_step(st);
      autoshow.set_buy_confirm_open(false);
      autoshow.set_show_specs(show_specs);
      carview::ui::List lst;
      carview::draw_autoshow_hud(lst, 1280.0f, 720.0f, autoshow, bands, credits);
      const bool show_filters = (st == carview::AutoshowStep::kCarSelect && !autoshow.tutorial_mode());
      const auto lo = carview::compute_autoshow_layout(1280.0f, 720.0f, st, show_specs, show_filters);
      CHECK(lo.hero_car_viewport.valid());
      CHECK(lo.hero_car_viewport.width() >= 760.0f);
      CHECK(lo.hero_car_viewport.height() >= 360.0f);
      CHECK(!lst.hits().empty());
      for (const auto& hz : lst.hits()) {
        carview::AutoshowRect r{hz.x0, hz.y0, hz.x1, hz.y1};
        CHECK(!r.intersects(lo.hero_car_viewport));
      }
      for (const auto& it : lst.items()) {
        carview::AutoshowRect r{it.rect[0], it.rect[1], it.rect[2], it.rect[3]};
        CHECK(!r.intersects(lo.hero_car_viewport));
      }
    };
    autoshow.set_step(carview::AutoshowStep::kMakeSelect);
    autoshow.select_make(1, credits);  // "All Cars"
    check_non_clipping_draw(carview::AutoshowStep::kMakeSelect, true);
    check_non_clipping_draw(carview::AutoshowStep::kMakeSelect, false);
    check_non_clipping_draw(carview::AutoshowStep::kCarSelect, true);
    check_non_clipping_draw(carview::AutoshowStep::kCarSelect, false);
    check_non_clipping_draw(carview::AutoshowStep::kColorSelect, true);
    check_non_clipping_draw(carview::AutoshowStep::kColorSelect, false);
    autoshow.set_show_specs(true);

    // Spec comparison delta calculation against explicit baseline & live Garage current car.
    carview::AutoshowComparisonStats base{};
    base.valid = true;
    base.car_id = 309;
    base.display_name = "1995 VW Corrado VR6";
    base.class_id = 2;
    base.shown_pi = 282;
    base.raw_pi = 0.524381;
    base.speed_rating = 4.2;
    base.handling_rating = 4.5;
    base.accel_rating = 4.0;
    base.launch_rating = 4.1;
    base.braking_rating = 4.3;
    base.peak_power_kw = 132.0;
    base.peak_torque_nm = 235.0;
    base.curb_weight_kg = 1240.0;
    autoshow.set_comparison_car(base);
    const carview::AutoshowCar* cand = autoshow.selected_car();
    CHECK(cand != nullptr);
    if (cand) {
      const auto delta = autoshow.compare_car(*cand, bands);
      CHECK(delta.has_baseline);
      const int cand_pi = garage::shown_index(cand->dealer.performance_index, bands);
      const int base_pi = garage::shown_index(base.raw_pi, bands);
      CHECK(delta.pi_delta == cand_pi - base_pi);
      CHECK(std::fabs(delta.speed_delta - (cand->speed_rating - base.speed_rating)) < 1e-6);
      CHECK(std::fabs(delta.handling_delta - (cand->handling_rating - base.handling_rating)) < 1e-6);
      CHECK(std::fabs(delta.accel_delta - (cand->accel_rating - base.accel_rating)) < 1e-6);
      CHECK(std::fabs(delta.launch_delta - (cand->launch_rating - base.launch_rating)) < 1e-6);
      CHECK(std::fabs(delta.braking_delta - (cand->braking_rating - base.braking_rating)) < 1e-6);
      CHECK(std::fabs(delta.power_kw_delta - (cand->peak_power_kw - base.peak_power_kw)) < 1e-6);
      CHECK(std::fabs(delta.torque_nm_delta - (cand->peak_torque_nm - base.peak_torque_nm)) < 1e-6);
      CHECK(std::fabs(delta.weight_kg_delta - (cand->curb_weight_kg - base.curb_weight_kg)) < 1e-6);
    }

    // Manufacturer/model page jumps, breadcrumbs, paint finishes, and caliper options.
    autoshow.set_step(carview::AutoshowStep::kMakeSelect);
    CHECK(autoshow.breadcrumb_text() == "MANUFACTURERS");
    const size_t mk0 = autoshow.selected_make_index();
    autoshow.page_move_make(1, credits, 6);
    CHECK(autoshow.selected_make_index() != mk0);
    autoshow.select_make(1, credits);  // "All Cars"
    CHECK(!autoshow.advance_step());
    CHECK(autoshow.step() == carview::AutoshowStep::kCarSelect);
    CHECK(autoshow.breadcrumb_text().find("MANUFACTURERS  >  ") == 0);
    if (autoshow.visible_cars().size() > 5) {
      const size_t c0 = autoshow.selected_index();
      autoshow.page_move_car(1, 5);
      CHECK(autoshow.selected_index() != c0);
    }
    if (autoshow.selected_car() && !autoshow.selected_car()->swatches.empty()) {
      CHECK(!autoshow.advance_step());
      CHECK(autoshow.step() == carview::AutoshowStep::kColorSelect);
      CHECK(autoshow.breadcrumb_text().find("SELECT PAINT") != std::string::npos);

      autoshow.cycle_paint_finish(1);
      CHECK(autoshow.paint_finish() == carview::AutoshowPaintFinish::kGloss);
      autoshow.set_paint_finish(carview::AutoshowPaintFinish::kMatte);
      autoshow.cycle_caliper_option(1);
      CHECK(autoshow.selected_caliper().active);
      const cars::CarPaintSpec pspec = autoshow.preview_paint_spec();
      CHECK(pspec.body.active && pspec.body.finish == cars::PaintFinish::kMatte);
      CHECK(pspec.calipers.active && pspec.calipers.rgb == autoshow.selected_caliper().rgb);
    }

    // Affordable / unaffordable purchase guards.
    const carview::AutoshowCar* buy_target = autoshow.selected_car();
    CHECK(buy_target != nullptr);
    if (buy_target) {
      const int64_t cost = buy_target->dealer.base_cost;
      const int64_t poor_cr = std::max<int64_t>(0, cost - 1);
      CHECK(!autoshow.can_afford_selected(poor_cr));
      CHECK(autoshow.credits_shortfall(poor_cr) == cost - poor_cr);
      std::string guard_msg;
      CHECK(!autoshow.request_buy_confirm(poor_cr, guard_msg));
      CHECK(!autoshow.buy_confirm_open());
      CHECK(guard_msg.find("INSUFFICIENT") != std::string::npos);

      int64_t poor_wallet = poor_cr;
      const size_t garage_before = g.cars.size();
      std::string fail_media, fail_msg;
      CHECK(!autoshow.buy(db, g, poor_wallet, fail_media, fail_msg));
      CHECK(poor_wallet == poor_cr);
      CHECK(g.cars.size() == garage_before);

      const int64_t rich_cr = cost + 50000;
      CHECK(autoshow.can_afford_selected(rich_cr));
      CHECK(autoshow.credits_shortfall(rich_cr) == 0);
      CHECK(autoshow.request_buy_confirm(rich_cr, guard_msg));
      CHECK(autoshow.buy_confirm_open());
      CHECK(autoshow.breadcrumb_text().find("CONFIRM PURCHASE") != std::string::npos);

      // Confirm modal emits both kHitConfirm and kHitCancel hit zones.
      carview::ui::List l_modal;
      carview::draw_autoshow_hud(l_modal, 1280.0f, 720.0f, autoshow, bands, rich_cr);
      bool saw_confirm = false, saw_cancel = false;
      for (const auto& hz : l_modal.hits()) {
        if (hz.group == carview::ui::kHitConfirm) saw_confirm = true;
        if (hz.group == carview::ui::kHitCancel) saw_cancel = true;
      }
      CHECK(saw_confirm && saw_cancel);

      // Backing out closes the confirmation modal cleanly before leaving Color Select.
      CHECK(autoshow.back_step());
      CHECK(!autoshow.buy_confirm_open());
      CHECK(autoshow.step() == carview::AutoshowStep::kColorSelect);
    }
  }
}

void test_autoshow_layout_and_studio_camera() {
  // 1. The original's dark Autoshow room (19_autoshow_orig.png).
  const auto bg = carview::hub_background_color(carview::HubRoom::kAutoshow);
  CHECK(bg[0] <= 0.10f && bg[1] <= 0.10f && bg[2] <= 0.12f);

  // 2. Smooth exponential camera transitions between Overview 3/4, Close Paint, manual orbit, and idle resume.
  carview::HubCamera cam;
  cam.set_focus(carview::HubCameraFocus::kTurntable);
  const float overview_dist = cam.target_dist;
  cam.set_focus(carview::HubCameraFocus::kPaint);
  CHECK(cam.target_dist < overview_dist);  // closer paint angle
  const float start_dist = cam.distance;
  cam.step(0.15f);
  CHECK(cam.distance < start_dist && cam.distance > cam.target_dist);  // smooth exponential damping, no snap!
  cam.nudge_orbit(0.4f, 0.1f);
  CHECK(!cam.auto_orbit && cam.user_orbited);
  // Advance past idle delay -> gentle auto-orbit resumes cleanly.
  for (int i = 0; i < 40; ++i) cam.step(0.1f);
  CHECK(cam.auto_orbit);

  // Framing lens shift smoothly damps and updates Autoshow projection matrix.
  cam.set_framing(0.11f, 0.16f);
  for (int i = 0; i < 20; ++i) cam.step(0.05f);
  CHECK(std::fabs(cam.lens_shift_x - 0.11f) < 0.01f);
  CHECK(std::fabs(cam.lens_shift_y - 0.16f) < 0.01f);
  carview::ShowroomFrame as_frame{};
  carview::build_hub_frame(carview::HubRoom::kAutoshow, cam, 1280, 720, as_frame, false);
  CHECK(as_frame.drive[3] == 0.0f);
  CHECK(as_frame.options[0] >= 0.50f && as_frame.options[0] <= 0.65f);
  CHECK(as_frame.options[1] >= 0.95f);

  // 3. Non-clipping layout geometry across 16:9, 21:9, and 4K resolutions & all steps.
  const std::pair<float, float> resolutions[] = {
      {1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1080.0f}, {3840.0f, 2160.0f}};
  for (auto [rw, rh] : resolutions) {
    for (auto st : {carview::AutoshowStep::kMakeSelect, carview::AutoshowStep::kCarSelect,
                    carview::AutoshowStep::kColorSelect}) {
      for (bool specs : {true, false}) {
        const auto lo = carview::compute_autoshow_layout(rw, rh, st, specs, st == carview::AutoshowStep::kCarSelect);
        CHECK(lo.hero_car_viewport.valid());
        CHECK(!lo.top_bar.intersects(lo.hero_car_viewport));
        CHECK(!lo.filter_bar.intersects(lo.hero_car_viewport));
        CHECK(!lo.spec_card.intersects(lo.hero_car_viewport));
        CHECK(!lo.rail_header.intersects(lo.hero_car_viewport));
        CHECK(!lo.carousel_rail.intersects(lo.hero_car_viewport));
        CHECK(!lo.swatch_strip.intersects(lo.hero_car_viewport));
        CHECK(!lo.finish_strip.intersects(lo.hero_car_viewport));
        CHECK(!lo.button_bar.intersects(lo.hero_car_viewport));
      }
    }
  }
}

void test_garage_restoration_bay_and_remove_car_standalone() {
  garage::Garage g;
  g.last_id = 2;
  g.selected = 1;
  garage::Car active_car;
  active_car.id = 1;
  active_car.car_id = 309;
  active_car.performance_index = 0.45;
  active_car.class_id = 2;
  garage::Car prize_car;
  prize_car.id = 2;
  prize_car.car_id = 445;
  prize_car.performance_index = 0.72;
  prize_car.class_id = 5;
  prize_car.is_prize = true;
  g.cars = {active_car, prize_car};

  std::vector<garage::ClassBand> bands = {
      {0.2, 200, "E"}, {0.35, 300, "D"}, {0.5, 400, "C"}, {0.65, 500, "B"}, {0.8, 600, "A"}, {1.0, 999, "R1"}};
  carview::GarageBrowser gb;
  gb.refresh(g, nullptr, bands);
  CHECK(gb.visible().size() == 2);

  // Select the prize car and confirm two-step REMOVE CAR yields 0 CR.
  for (size_t i = 0; i < gb.visible().size(); ++i) {
    if (gb.visible()[i].garage_id == 2) {
      gb.select_index(i);
      break;
    }
  }
  int64_t cr = 50000;
  std::string msg;
  CHECK(!gb.remove_selected(g, nullptr, bands, cr, msg));
  CHECK(gb.remove_confirm_garage_id() == 2);
  CHECK(gb.remove_selected(g, nullptr, bands, cr, msg));
  CHECK(cr == 50000);
  CHECK(g.cars.size() == 1);
  // Last remaining car cannot be removed.
  CHECK(!gb.remove_selected(g, nullptr, bands, cr, msg, /*confirmed=*/true, /*allow_switch_active=*/true));
  CHECK(g.cars.size() == 1);

  // Restoration Bay: in-progress blocked; READY TO CLAIM invokes award callback with is_prize = true.
  gb.set_restorations({{"barnfind_01", "1956 Jaguar D-Type", 901, 45.0, 15.0, false},
                       {"barnfind_02", "1964 Aston Martin DB5", 902, 45.0, 45.0, true}});
  gb.set_tab(carview::GarageTab::kRestorationBay);
  std::string claimed_id;
  CHECK(!gb.claim_selected_restoration(g, nullptr, bands, claimed_id, msg));
  gb.move(1);
  bool saw_prize_flag = false;
  CHECK(gb.claim_selected_restoration(
      g, nullptr, bands,
      [&](int64_t cid, int /*col*/, bool /*sel*/, bool is_prize) -> std::string {
        saw_prize_flag = is_prize && cid == 902;
        garage::Car nc;
        nc.id = ++g.last_id;
        nc.car_id = cid;
        nc.is_prize = is_prize;
        g.cars.push_back(nc);
        return "1964 Aston Martin DB5";
      },
      claimed_id, msg));
  CHECK(saw_prize_flag && claimed_id == "barnfind_02" && gb.restorations().size() == 1 && g.cars.size() == 2 &&
        g.cars.back().is_prize);

  carview::ui::List l_bay;
  carview::draw_garage_hud(l_bay, 1280.0f, 720.0f, gb, bands, cr);
  CHECK(!l_bay.empty());
}

}  // namespace

int main() {
  test_hub_stage_and_camera();
  test_autoshow_layout_and_studio_camera();
  test_livery_editor_groups_undo_and_catalog();
  test_garage_restoration_bay_and_remove_car_standalone();
  test_disc_hub_shops_and_garage();
  if (g_failed) {
    std::fprintf(stderr, "hub_shop_tests: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("hub_shop_tests: all passed\n");
  return 0;
}
