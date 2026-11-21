// The career garage (SPEC-GARAGE-UPGRADES): the owned cars with their
// fitted parts, the parts bought, the selected car; part prices with the
// sign discount, buying and refitting, the class bands and the shown
// index. A small versioned text file of our own beside the drive save:
//
//   shift-garage 1
//   last 38
//   selected 12
//   car 12 445 0.809454 6 6480 1 Brakes=445003 TireCompound=445002 ...
//   bought 12 4 445003 0 1258
//
// "car" is the garage Id, the Data_Car id, the index, the class, the parts
// value, the paint (DefaultManufacturerColorIndex) and each slot's fitted
// row (Career_Garage's column names; -1 slots left out). "bought" is a
// Career_PurchasedParts row: GarageId, PartEnum, PartId, ManufacturerId,
// PricePaid.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "cars/physics.h"

namespace garage {

struct Car {
  int64_t id = 0, car_id = 0;
  double performance_index = 0;  // raw, (0, 1]
  int class_id = 0;
  int64_t parts_value = 0;
  int colour = 0;
  std::map<std::string, int64_t> slots;  // Career_Garage column -> fitted row Id
};

struct Bought {
  int64_t garage_id = 0;
  int part_enum = 0;
  int64_t part_id = 0, manufacturer = 0, price_paid = 0;
};

struct Garage {
  std::vector<Car> cars;  // by Id
  std::vector<Bought> bought;
  int64_t last_id = 0;
  int64_t selected = 0;

  Car* find(int64_t id);
  const Car* find(int64_t id) const;
  const Car* selected_car() const;  // the selected row, else the lowest Id
  bool owns_part(int64_t garage_id, int part_enum, int64_t part_id) const;
};

// The fitted rows of a car as the car loader takes them (table -> row Id).
cars::FittedParts fitted(const Car& car);

// SPEC-GARAGE-UPGRADES, Prices and the sign discount: p' = floor((1 - d) p
// + 0.5) in single precision with d = discount / 100; p when d is 0.
int64_t discounted_price(int64_t price, int discount_percent);

// Fits `part` on car `garage_id`: free when the car bought it before (no
// new row), else charged at the discounted price (refused, false, when
// `credits` is short). A bought part adds a row and its price to the car's
// parts value.
struct FitResult {
  bool ok = false;
  int64_t charged = 0;
  bool refit = false;
};
FitResult fit_part(Garage& g, int64_t garage_id, const cars::PartRow& part, const char* column, int discount_percent,
                   int64_t& credits);

// Buys a car (SPEC-GARAGE-UPGRADES, A purchase): refused (false) when
// `credits` < `price`; else the credits pay, a row with Id last + 1, the
// listed index and class, `stock` slots, parts value 0 and `colour` is
// added, and it becomes the selected car. `new_id` gets the Id.
bool buy_car(Garage& g, int64_t car_id, int64_t price, double performance_index, int class_id, int colour,
             const std::map<std::string, int64_t>& stock, int64_t& credits, int64_t* new_id = nullptr);

// The class bands (CarClasses: raw and shown ceilings, by Id).
struct ClassBand {
  double raw_max = 0, shown_max = 0;
  std::string name;
};
// The class Id of a raw index: the lowest band whose raw ceiling is at or
// above it (the last band above all).
int class_of(double raw, const std::vector<ClassBand>& bands);
// The shown index, interpolated inside the band and rounded up.
int shown_index(double raw, const std::vector<ClassBand>& bands);
// The disc's CarClasses (Id, MaxPerformanceIndex, MaxDisplayPerformanceIndex).
bool load_class_bands(const std::filesystem::path& game_root, std::vector<ClassBand>& out, std::string* error);

std::string to_text(const Garage& g);
bool from_text(const std::string& text, Garage& out, std::string* error);
bool read_garage(const std::filesystem::path& path, Garage& out, std::string* error);  // false, empty error: no file
bool write_garage(const std::filesystem::path& path, const Garage& g, std::string* error);

}  // namespace garage
