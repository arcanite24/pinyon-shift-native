// SPEC-GARAGE-UPGRADES synthetic cases: prices with the sign discount,
// buying and refitting parts, the class bands and the shown index.
#include <cstdio>

#include "garage/garage.h"

static int failures = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
      ++failures;                                                   \
    }                                                               \
  } while (0)

int main() {
  using namespace garage;
  // Prices and the sign discount.
  CHECK(discounted_price(7400, 0) == 7400);
  CHECK(discounted_price(7400, 83) == 1258);
  CHECK(discounted_price(2000, 83) == 340);
  CHECK(discounted_price(1600, 83) == 272);
  CHECK(discounted_price(10000, 85) == 1500);
  CHECK(discounted_price(123456, 100) == 0);

  // Buy, refuse, refit.
  Garage g;
  g.cars.push_back({1, 309, 0.5, 2, 0, 0, {{"Brakes", 309000}}});
  cars::PartRow brakes{4, "List_UpgradeBrakes", 309003, 3, false, 10000, 7};
  int64_t credits = 1499;
  CHECK(!fit_part(g, 1, brakes, "Brakes", 85, credits).ok && credits == 1499 && g.bought.empty());
  credits = 60000;
  FitResult r = fit_part(g, 1, brakes, "Brakes", 85, credits);
  CHECK(r.ok && r.charged == 1500 && credits == 58500 && g.bought.size() == 1 && g.cars[0].parts_value == 1500);
  CHECK(g.cars[0].slots["Brakes"] == 309003);
  cars::PartRow stock{4, "List_UpgradeBrakes", 309000, 0, true, 0, 0};
  r = fit_part(g, 1, stock, "Brakes", 85, credits);
  CHECK(r.ok && r.refit && credits == 58500 && g.bought.size() == 1);
  r = fit_part(g, 1, brakes, "Brakes", 85, credits);
  CHECK(r.ok && r.refit && r.charged == 0 && credits == 58500 && g.bought.size() == 1 && g.cars[0].parts_value == 1500);
  CHECK(fitted(g.cars[0]).at("List_UpgradeBrakes") == 309003);
  g.cars[0].slots["WheelStyle"] = 5007;
  CHECK(fitted(g.cars[0]).at("List_Wheels") == 5007);

  // Buying a car.
  {
    Garage b;
    b.last_id = 7;
    b.cars.push_back({7, 309, 0.5, 2, 0, 0, {}});
    int64_t cr = 51999, id = 0;
    CHECK(!buy_car(b, 1009, 52000, 0.6, 4, 1, {{"Brakes", 1009000}}, cr, &id) && cr == 51999 && b.cars.size() == 1);
    cr = 60000;
    CHECK(buy_car(b, 1009, 52000, 0.6, 4, 1, {{"Brakes", 1009000}}, cr, &id) && cr == 8000 && id == 8);
    CHECK(b.last_id == 8 && b.selected == 8 && b.cars.back().parts_value == 0 && b.cars.back().slots["Brakes"] == 1009000);
  }

  // Class bands (the disc's CarClasses ceilings).
  const std::vector<ClassBand> bands = {{0.005, 99, "F"},   {0.459, 200, "E"},  {0.539, 300, "D"},
                                        {0.5975, 400, "C"}, {0.6505, 500, "B"}, {0.739, 600, "A"},
                                        {0.81, 700, "S"},   {0.8665, 800, "R3"}, {0.8991, 900, "R2"},
                                        {0.999999999999, 999, "R1"}, {1.0, 999, "X"}};
  CHECK(class_of(0.5975, bands) == 3 && shown_index(0.5975, bands) == 400);
  CHECK(class_of(0.6505, bands) == 4 && shown_index(0.6505, bands) == 500);
  CHECK(class_of(0.6506, bands) == 5);
  CHECK(shown_index(0.72, bands) == 579);
  CHECK(shown_index(0.61666, bands) == 437);
  CHECK(shown_index(0.94186, bands) == 942);

  // The text file round trip.
  Garage back;
  std::string e;
  CHECK(from_text(to_text(g), back, &e) && back.cars.size() == 1 && back.bought.size() == 1 &&
        back.cars[0].slots["Brakes"] == 309003 && back.bought[0].price_paid == 1500);

  // Retail FH1 discount non-stacking: Discount Signs (0..100%) ONLY, Affinity never stacks on upgrades.
  CHECK(upgrade_discount_percent(0, 50) == 0);
  CHECK(upgrade_discount_percent(15, 50) == 15);
  CHECK(upgrade_discount_percent(83, 100) == 83);
  CHECK(upgrade_discount_percent(100, 50) == 100);
  CHECK(upgrade_discount_percent(140, 25) == 100);
  CHECK(upgrade_part_price(10000, 15, 50) == 8500);
  CHECK(upgrade_part_price(10000, 100, 0) == 0);

  // Retail prize-car sell-value rule: prize/gift/barn-find cars sell for 100 CR; purchased cars sell for 50%.
  CHECK(sell_car_value(250000, 30000, false) == 140000);
  CHECK(sell_car_value(250000, 30000, true) == kPrizeCarSellValue);
  CHECK(kPrizeCarSellValue == 100);
  {
    Garage pg;
    int64_t cr = 100000, purchased_id = 0, prize_id = 0;
    CHECK(buy_car(pg, 309, 20000, 0.52, 2, 1, {{"Brakes", 309000}}, cr, &purchased_id));
    CHECK(cr == 80000);
    CHECK(award_prize_car(pg, 445, 0.72, 5, 1, {{"Brakes", 445000}}, &prize_id));
    CHECK(cr == 80000);  // prize car costs 0 CR
    CHECK(pg.find(prize_id) != nullptr && pg.find(prize_id)->is_prize);
    pg.find(prize_id)->parts_value = 40000;
    pg.selected = purchased_id;
    auto sp_prize = sell_car(pg, prize_id, 200000, cr);
    CHECK(sp_prize.ok && sp_prize.payout == 100 && cr == 80100);
  }

  // Orphaned sub-part removal when selling FI conversion or engine swap + explicit NA revert.
  {
    Garage og;
    og.last_id = 1;
    og.selected = 1;
    Car c;
    c.id = 1;
    c.car_id = 309;
    c.performance_index = 0.65;
    c.class_id = 4;
    c.slots["Engine"] = 309001;
    c.slots["Camshaft"] = 999102;
    c.slots["SingleTurbo"] = 999201;
    c.slots["Intercooler"] = 999301;
    c.parts_value = 26000;
    og.cars.push_back(c);
    og.bought.push_back({1, 0, 309001, 0, 12000});   // Engine swap
    og.bought.push_back({1, 14, 999102, 0, 4000});   // Camshaft
    og.bought.push_back({1, 24, 999201, 0, 6000});   // SingleTurbo conversion
    og.bought.push_back({1, 29, 999301, 0, 4000});   // Intercooler dependent on FI

    int64_t cr = 0;
    // Selling SingleTurbo conversion (-1 stock) removes orphaned Intercooler too.
    auto sfi = sell_part(og, 1, 24, 999201, -1, "SingleTurbo", cr);
    CHECK(sfi.ok && sfi.orphans_removed == 1 && sfi.payout == 5000 && cr == 5000);
    CHECK(og.find(1)->slots["SingleTurbo"] == -1);
    CHECK(og.find(1)->slots["Intercooler"] == -1);
    CHECK(og.bought.size() == 2);

    // Selling Engine swap removes orphaned Camshaft sub-part too.
    auto seng = sell_part(og, 1, 0, 309001, 309000, "Engine", cr);
    CHECK(seng.ok && seng.orphans_removed == 1 && seng.payout == 8000 && cr == 13000);
    CHECK(og.find(1)->slots["Engine"] == 309000);
    CHECK(og.bought.empty());

    // Explicit NA revert clears all FI + Intercooler slots cleanly.
    og.find(1)->slots["TwinTurbo"] = 888101;
    og.find(1)->slots["Intercooler"] = 888201;
    CHECK(revert_to_naturally_aspirated(og, nullptr, 1));
    CHECK(og.find(1)->slots["TwinTurbo"] == -1 && og.find(1)->slots["Intercooler"] == -1);
    CHECK(!revert_to_naturally_aspirated(og, nullptr, 1));  // already NA
  }

  // Favorites, prize flag, and Career_Garage.Id-keyed Car Club sharing + borrow/return persistence.
  {
    Garage cg;
    int64_t cr = 500000, gid1 = 0, gid2 = 0;
    CHECK(buy_car(cg, 309, 10000, 0.52, 2, 1, {{"Brakes", 309000}}, cr, &gid1));
    CHECK(buy_car(cg, 309, 10000, 0.70, 5, 2, {{"Brakes", 309003}}, cr, &gid2));  // same Data_Car.Id, distinct instance
    CHECK(gid1 != gid2);
    CHECK(cg.toggle_favorite(gid1) && cg.find(gid1)->favorite && !cg.find(gid2)->favorite);
    cg.find(gid2)->is_prize = true;

    // Sharing gid1 must NOT mark gid2 (same Data_Car.Id) as shared!
    CHECK(cg.toggle_club_shared(gid1));
    CHECK(cg.is_club_shared(gid1) && !cg.is_club_shared(gid2));
    cg.selected = gid2;
    CHECK(cg.borrow_club_car(gid1));
    CHECK(cg.club_borrowed_garage_id == gid1 && cg.club_return_garage_id == gid2 && cg.selected == gid1);

    // Round-trip garage.txt persistence preserves favorites, prize flags, club_shared, and club_borrowed.
    Garage cg_rt;
    std::string rerr;
    CHECK(from_text(to_text(cg), cg_rt, &rerr));
    CHECK(cg_rt.find(gid1) != nullptr && cg_rt.find(gid1)->favorite && !cg_rt.find(gid1)->is_prize);
    CHECK(cg_rt.find(gid2) != nullptr && !cg_rt.find(gid2)->favorite && cg_rt.find(gid2)->is_prize);
    CHECK(cg_rt.is_club_shared(gid1) && !cg_rt.is_club_shared(gid2));
    CHECK(cg_rt.club_borrowed_garage_id == gid1 && cg_rt.club_return_garage_id == gid2);

    CHECK(cg_rt.return_borrowed_club_car());
    CHECK(cg_rt.club_borrowed_garage_id == 0 && cg_rt.selected == gid2);

    // Selling a shared car removes its instance ID from club_shared_garage_ids.
    auto sold = sell_car(cg_rt, gid1, 10000, cr);
    CHECK(sold.ok && !cg_rt.is_club_shared(gid1));
  }

  if (failures == 0) std::printf("garage_tests: all passed\n");
  return failures ? 1 : 0;
}
