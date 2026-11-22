// The career garage (SPEC-GARAGE-UPGRADES): owned cars with their fitted
// parts, purchased parts, conversions (engine/drivetrain/body/aspiration),
// Auto Upgrade, car/part selling, car tuning sliders, livery reference,
// PI.xml virtual-lap recalculation, class bands and persistence.
//
// Format (garage.txt, version 1 and 2; v2 adds optional `tune` and `livery` lines):
//   shift-garage 2
//   last 38
//   selected 12
//   car 12 445 0.809454 6 6480 1 Brakes=445003 TireCompound=445002 ...
//   tune 12 Tuning_frontTirePressure=29.5 ...
//   livery 12 liveries/12.liv
//   bought 12 4 445003 0 1258
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "drive/from_physics.h"
#include "garage/pi_calc.h"

namespace garage {

struct Car {
  int64_t id = 0, car_id = 0;
  double performance_index = 0;  // raw, (0, 1]
  int class_id = 0;
  int64_t parts_value = 0;
  int colour = 0;
  std::map<std::string, int64_t> slots;  // Career_Garage column -> fitted row Id (-1 = absent)
  drive::CarTune tune;                   // 35 Tuning_* sliders (default -1 / -100 = stock)
  std::string livery_file;               // optional relative livery path (e.g. "liveries/12.liv")
  CarStats cached_stats;                 // recomputed whenever parts change
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
  const Bought* find_bought(int64_t garage_id, int part_enum, int64_t part_id) const;
};

// The fitted rows of a car as the car loader takes them (table -> row Id).
// Preserves explicit -1 entries on optional aspiration tables so conversions override stock induction.
cars::FittedParts fitted(const Car& car);

// SPEC-GARAGE-UPGRADES, Prices and the sign discount: p' = floor((1 - d) p
// + 0.5) in single precision with d = discount / 100; p when d is 0.
int64_t discounted_price(int64_t price, int discount_percent);

struct FitResult {
  bool ok = false;
  int64_t charged = 0;
  bool refit = false;
  bool rekeyed = false;  // true when an engine/drivetrain/body/aspiration conversion re-keyed sub-parts
};

// Fits `part` on car `garage_id` (simple single-slot fit, backwards-compatible).
FitResult fit_part(Garage& g, int64_t garage_id, const cars::PartRow& part, const char* column, int discount_percent,
                   int64_t& credits);

// Full fit supporting Engine (0), Drivetrain (1), CarBody (2), and Aspiration (24..28)
// conversions: re-keys dependent sub-part slots to the new family's stock rows, clears
// conflicting forced-induction slots to -1, and recomputes PI, class_id, and stats.
FitResult fit_part_full(Garage& g, const cars::PartsDb& db, int64_t garage_id, const cars::PartRow& part,
                        const char* column, int discount_percent, int64_t& credits,
                        const cars::PhysicsSettings* phys_settings = nullptr, const PiConfig* pi_cfg = nullptr,
                        const std::vector<ClassBand>* bands = nullptr, PiResult* pi_out = nullptr);

// Recomputes and updates `car.performance_index`, `car.class_id`, and `car.cached_stats`.
bool refresh_car_pi(Car& car, const cars::PartsDb& db, const cars::PhysicsSettings& phys_settings,
                    const PiConfig& pi_cfg, const std::vector<ClassBand>& bands, PiResult* out = nullptr,
                    std::string* error = nullptr);

// Buys a car (SPEC-GARAGE-UPGRADES, A purchase).
bool buy_car(Garage& g, int64_t car_id, int64_t price, double performance_index, int class_id, int colour,
             const std::map<std::string, int64_t>& stock, int64_t& credits, int64_t* new_id = nullptr);

// Selling cars (SPEC-GARAGE-UPGRADES, Selling): payout = floor(0.5 * (base_cost + parts_value)).
// Refuses to sell the currently selected car (`g.selected`) or the last remaining car.
int64_t sell_car_value(int64_t base_cost, int64_t parts_value);
struct SellCarResult {
  bool ok = false;
  int64_t payout = 0;
  std::string reason;
};
SellCarResult sell_car(Garage& g, int64_t garage_id, int64_t base_cost, int64_t& credits);

// Selling purchased parts (RestockingValueScale = 0.5 of PricePaid).
// Refuses stock parts or unowned parts. If currently fitted, reverts slot to `stock_part_id`.
int64_t sell_part_value(int64_t price_paid);
struct SellPartResult {
  bool ok = false;
  int64_t payout = 0;
  std::string reason;
};
SellPartResult sell_part(Garage& g, int64_t garage_id, int part_enum, int64_t part_id, int64_t stock_part_id,
                         const char* column, int64_t& credits);

// Auto Upgrade (UpgradeWizardParts / UpgradeWizardPartsRemoval / UpgradeWizardPowerRemoval).
struct AutoUpgradeStep {
  cars::PartRow part;
  std::string column;
  int64_t cost = 0;
  bool removal = false;
};

struct AutoUpgradePlan {
  bool ok = false;
  int target_class_id = 0;
  double start_pi = 0.0;
  int start_class_id = 0;
  int start_shown_pi = 0;
  PiResult result;
  int64_t total_cost = 0;
  std::vector<AutoUpgradeStep> steps;
  std::string message;
};

// Plans upgrades (or removals when dropping a class) to fit `garage_id` to `target_class_id`
// within OptimizePIRange (0.003 raw under the class ceiling).
AutoUpgradePlan plan_auto_upgrade(const Garage& g, const cars::PartsDb& db, int64_t garage_id, int target_class_id,
                                  int discount_percent, const cars::PhysicsSettings& phys_settings,
                                  const PiConfig& pi_cfg, const std::vector<ClassBand>& bands);

// Applies a previously computed AutoUpgradePlan if `credits >= plan.total_cost`.
bool apply_auto_upgrade(Garage& g, const cars::PartsDb& db, int64_t garage_id, const AutoUpgradePlan& plan,
                        int discount_percent, int64_t& credits, const cars::PhysicsSettings& phys_settings,
                        const PiConfig& pi_cfg, const std::vector<ClassBand>& bands, std::string* message = nullptr);

// The class bands (CarClasses: raw and shown ceilings, by Id).
struct ClassBand {
  double raw_max = 0, shown_max = 0;
  std::string name;
};
int class_of(double raw, const std::vector<ClassBand>& bands);
int shown_index(double raw, const std::vector<ClassBand>& bands);
bool load_class_bands(const std::filesystem::path& game_root, std::vector<ClassBand>& out, std::string* error);

std::string to_text(const Garage& g);
bool from_text(const std::string& text, Garage& out, std::string* error);
bool read_garage(const std::filesystem::path& path, Garage& out, std::string* error);  // false, empty error: no file
bool write_garage(const std::filesystem::path& path, const Garage& g, std::string* error);

}  // namespace garage
