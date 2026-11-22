// Unit tests for Slice 7 Milestone 3: 3D Festival Hub screens (uiautoshow Make
// Carousel + 2-car tutorial + factory colour picker, uiworkshop / Dak's Garage
// 8-category UpgradeShop with Conversions/Auto-Upgrade/35-slider Tuning/50%
// Sell Part & live PI/5-stat diffs, GarageBrowser sort/filter/Sell Car, 3D hub
// studio plinth + drop-shadow stage, and Barn Find restoration card HUD).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "autoshow.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "hub_scene.h"
#include "ui.h"
#include "ui4_chrome.h"
#include "upgrade_shop.h"

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

  // 1. Autoshow Make Carousel + "All Affordable" + "no affordable cars" + factory colour swatch picker.
  carview::Autoshow autoshow;
  CHECK(autoshow.open(db, {}, err, 0, false));
  CHECK(!autoshow.makes().empty());
  CHECK(autoshow.makes()[0].name == "All Affordable");
  CHECK(autoshow.visible_cars().empty());
  CHECK(autoshow.empty_reason() == "no affordable cars");

  // Move to a real make (alphabetical after "All Affordable").
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

  // 3. UpgradeShop: all 8 categories, aftermarket rims, conversions, Auto Upgrade, 35-slider Tuning, Sell Part.
  carview::UpgradeShop shop;
  CHECK(shop.open(db, g, gid309, err, &phys_settings, &pi_cfg, &bands));
  CHECK(shop.has_preview_pi());
  CHECK(shop.current_pi().shown_pi == 282);

  // Check Tires & Rims contains aftermarket rims (part_enum 43).
  shop.set_category(carview::ShopCategory::kTiresRims);
  const auto tire_rows = shop.visible_rows(0);
  bool has_rims = false;
  for (const auto& r : tire_rows)
    if (r.part_enum == 43) has_rims = true;
  CHECK(has_rims);

  // Check Conversions contains engine swaps (part_enum 0).
  shop.set_category(carview::ShopCategory::kConversions);
  const auto conv_rows = shop.visible_rows(0);
  CHECK(!conv_rows.empty());

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

  // Check Auto Upgrade tab -> A class.
  shop.set_category(carview::ShopCategory::kAutoUpgrade);
  const auto auto_rows = shop.visible_rows(0);
  CHECK(auto_rows.size() == 9);
  while (shop.selected_row() < 4) shop.move_slot(1);  // index 4 = class 5 (A)
  CHECK(shop.auto_plan().ok && shop.auto_plan().result.class_id == 5);
  CHECK(shop.fit(0, credits, msg));
  CHECK(g.find(gid309)->class_id == 5);

  // Check Tuning tab (25..35 sliders depending on drivetrain/gear count) + adjust tire pressure.
  shop.set_category(carview::ShopCategory::kTuning);
  CHECK(shop.tuning_sliders().size() >= 24 && shop.tuning_sliders().size() <= 35);
  shop.move_part(1);
  CHECK(g.find(gid309)->tune.front_tire_pressure_psi > 0.0);

  // 4. GarageBrowser sort/filter + Sell Car (50% payout, active car protected).
  carview::GarageBrowser gb;
  gb.refresh(g, &db, bands);
  CHECK(gb.visible().size() == 3);
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kMake);
  gb.cycle_sort(1);
  CHECK(gb.sort_mode() == carview::GarageSort::kValue);

  // Select a non-active car and sell it for 50%.
  while (gb.selected_entry() && gb.selected_entry()->is_current) gb.move(1);
  const int64_t cr_before_sell_car = credits;
  CHECK(gb.sell_selected(g, &db, bands, credits, msg));
  CHECK(credits > cr_before_sell_car);
  CHECK(g.cars.size() == 2);

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
}

}  // namespace

int main() {
  test_hub_stage_and_camera();
  test_disc_hub_shops_and_garage();
  if (g_failed) {
    std::fprintf(stderr, "hub_shop_tests: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("hub_shop_tests: all passed\n");
  return 0;
}
