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

  if (failures == 0) std::printf("garage_tests: all passed\n");
  return failures ? 1 : 0;
}
