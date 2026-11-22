#include "garage/rally_kit.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "sqlite3.h"

namespace garage {
namespace {

const cars::UpgradeSlot* find_slot_by_enum(int part_enum) {
  for (const auto& s : cars::upgrade_slots())
    if (s.part_enum == part_enum) return &s;
  return nullptr;
}

std::string aero_slot_label(int part_enum) {
  switch (part_enum) {
    case 9: return "Rally Rear Wing";
    case 34: return "Rally Front Bumper / Light Pod";
    case 35: return "Rally Rear Bumper";
    case 36: return "Rally Side Skirts";
    case 37: return "Rally Hood / Light Pod";
    default: return "Rally Body Part";
  }
}

struct OptionalPart {
  int part_enum = 0;
  int64_t part_id = 0;
};

std::vector<OptionalPart> query_optional_rally_parts(const cars::PartsDb& db, int64_t car_id) {
  std::vector<OptionalPart> out;
  if (!db.is_open()) return out;
  std::string ignored;
  db.each("select PartEnum, PartId from OptionalRallyUpgrades where Ordinal = ? order by id",
          {std::to_string(car_id)},
          [&](sqlite3_stmt* st) {
            out.push_back({sqlite3_column_int(st, 0), sqlite3_column_int64(st, 1)});
          },
          &ignored);
  return out;
}

}  // namespace

bool is_rally_part(int part_enum, int level) {
  if (part_enum == 8 && level == 5) return true;   // List_UpgradeTireCompound: Rally compound 9
  if (part_enum == 5 && level == 4) return true;   // List_UpgradeSpringDamper: Rally suspension
  if (part_enum == 31 && level == 4) return true;  // List_UpgradeDrivetrainTransmission: Rally gearbox
  return false;
}

RallyKitStatus inspect_rally_kit(const Car& car, const cars::PartsDb& db) {
  RallyKitStatus st;
  if (!db.is_open()) return st;

  std::vector<cars::PartRow> rows;
  std::string ignored;
  if (db.options(car.car_id, fitted(car), rows, &ignored)) {
    auto slot_fitted_id = [&](const char* col) -> int64_t {
      auto it = car.slots.find(col);
      return it == car.slots.end() ? -1 : it->second;
    };
    const int64_t fit_tyre = slot_fitted_id("TireCompound");
    const int64_t fit_susp = slot_fitted_id("SpringDamper");
    const int64_t fit_trans = slot_fitted_id("Transmission");

    for (const auto& r : rows) {
      if (r.part_enum == 8 && r.level == 5) {
        st.can_convert = true;
        if (r.id == fit_tyre || (fit_tyre < 0 && r.is_stock)) st.has_rally_tyres = true;
      } else if (r.part_enum == 5 && r.level == 4) {
        if (r.id == fit_susp || (fit_susp < 0 && r.is_stock)) st.has_rally_suspension = true;
      } else if (r.part_enum == 31 && r.level == 4) {
        if (r.id == fit_trans || (fit_trans < 0 && r.is_stock)) st.has_rally_transmission = true;
      }
    }
  }

  // Fallback check: inspect fitted tyre compound directly in SQLite (catches compound 9 even on stock Rally cars).
  if (!st.has_rally_tyres) {
    auto it = car.slots.find("TireCompound");
    if (it != car.slots.end() && it->second >= 0) {
      db.each("select Level, TireCompoundID from List_UpgradeTireCompound where Id = ?", {std::to_string(it->second)},
              [&](sqlite3_stmt* s) {
                if (sqlite3_column_int(s, 0) == 5 || sqlite3_column_int(s, 1) == 9) st.has_rally_tyres = true;
              },
              &ignored);
    }
  }

  // Event-only Rally twins (1626..1635) come pre-configured as Rally cars.
  const bool is_rally_twin = (car.car_id >= 1626 && car.car_id <= 1635);
  if (is_rally_twin) {
    st.has_rally_tyres = true;
    st.has_rally_suspension = true;
    st.has_rally_transmission = true;
    st.can_convert = true;
  }

  // Optional Rally body parts (light pods, rally wings, etc.) for the 5 Rally cars.
  const auto opts = query_optional_rally_parts(db, car.car_id);
  std::set<int> seen_enums;
  for (const auto& opt : opts) {
    if (!seen_enums.insert(opt.part_enum).second) continue;
    ++st.optional_aero_total;
    if (const auto* slot = find_slot_by_enum(opt.part_enum)) {
      auto it = car.slots.find(slot->column);
      if (it != car.slots.end() && it->second == opt.part_id) ++st.optional_aero_fitted;
    }
  }
  st.has_optional_aero = st.optional_aero_total > 0;
  st.is_rally_upgraded = st.has_rally_tyres;
  st.is_full_kit = st.has_rally_tyres && st.has_rally_suspension && st.has_rally_transmission;
  return st;
}

RallyKitPlan plan_rally_kit(const Garage& g, const cars::PartsDb& db, int64_t garage_id, int discount_percent,
                            bool include_optional_aero, const cars::PhysicsSettings* phys_settings,
                            const PiConfig* pi_cfg, const std::vector<ClassBand>* bands) {
  RallyKitPlan plan;
  const Car* car = g.find(garage_id);
  if (!car || !db.is_open()) {
    plan.message = "car not available";
    return plan;
  }

  plan.start_pi = car->performance_index;
  plan.start_class_id = car->class_id;
  plan.start_shown_pi = bands ? shown_index(car->performance_index, *bands) : int(car->performance_index * 999.0);

  std::vector<cars::PartRow> rows;
  std::string why;
  if (!db.options(car->car_id, fitted(*car), rows, &why)) {
    plan.message = why;
    return plan;
  }

  auto add_candidate = [&](const cars::PartRow& r, const char* col, const std::string& label, bool opt_aero) {
    RallyKitStep step;
    step.part = r;
    step.column = col;
    step.label = label;
    step.optional_aero = opt_aero;
    auto it = car->slots.find(col);
    step.already_fitted = (it != car->slots.end() && it->second == r.id) || (it == car->slots.end() && r.is_stock);
    step.already_owned = step.already_fitted || r.is_stock || g.owns_part(garage_id, r.part_enum, r.id);
    step.cost = step.already_owned ? 0 : discounted_price(r.price, discount_percent);
    if (!step.already_fitted) plan.total_cost += step.cost;
    plan.steps.push_back(std::move(step));
  };

  // Core 3 Rally kit upgrades: Tyres (enum 8, L5), Suspension (enum 5, L4), Transmission (enum 31, L4).
  for (const auto& r : rows) {
    if (r.part_enum == 8 && r.level == 5) {
      add_candidate(r, "TireCompound", "Rally Tires (Compound 9)", false);
      break;
    }
  }
  for (const auto& r : rows) {
    if (r.part_enum == 5 && r.level == 4) {
      add_candidate(r, "SpringDamper", "Rally Suspension", false);
      break;
    }
  }
  for (const auto& r : rows) {
    if (r.part_enum == 31 && r.level == 4) {
      add_candidate(r, "Transmission", "Rally Transmission", false);
      break;
    }
  }

  if (include_optional_aero) {
    const auto opts = query_optional_rally_parts(db, car->car_id);
    std::set<int> added_enums;
    for (const auto& opt : opts) {
      if (!added_enums.insert(opt.part_enum).second) continue;
      const auto* slot = find_slot_by_enum(opt.part_enum);
      if (!slot) continue;
      for (const auto& r : rows) {
        if (r.part_enum == opt.part_enum && r.id == opt.part_id) {
          add_candidate(r, slot->column, aero_slot_label(opt.part_enum), true);
          break;
        }
      }
    }
  }

  if (plan.steps.empty()) {
    plan.message = "No Rally upgrades exist for this car";
    return plan;
  }

  bool any_to_fit = false;
  for (const auto& s : plan.steps)
    if (!s.already_fitted) any_to_fit = true;
  plan.already_complete = !any_to_fit;
  plan.ok = true;

  // Compute projected PI & stats with all Rally kit steps applied.
  if (phys_settings && pi_cfg && bands) {
    Car sim = *car;
    for (const auto& s : plan.steps) sim.slots[s.column] = s.part.id;
    refresh_car_pi(sim, db, *phys_settings, *pi_cfg, *bands, &plan.result, nullptr);
  } else {
    plan.result.raw_pi = car->performance_index;
    plan.result.class_id = car->class_id;
    plan.result.shown_pi = plan.start_shown_pi;
  }

  if (plan.already_complete) {
    plan.message = "Rally Kit already fitted";
  } else {
    char buf[128];
    std::snprintf(buf, sizeof buf, "Rally Kit (%zu parts) - %lld Cr", plan.steps.size(), (long long)plan.total_cost);
    plan.message = buf;
  }
  return plan;
}

bool apply_rally_kit(Garage& g, const cars::PartsDb& db, int64_t garage_id, const RallyKitPlan& plan,
                     int discount_percent, int64_t& credits, const cars::PhysicsSettings* phys_settings,
                     const PiConfig* pi_cfg, const std::vector<ClassBand>* bands, std::string* message) {
  if (!plan.ok) {
    if (message) *message = plan.message.empty() ? "Rally Kit unavailable" : plan.message;
    return false;
  }
  if (plan.already_complete) {
    if (message) *message = "Rally Kit already fitted";
    return true;
  }
  if (credits < plan.total_cost) {
    if (message) *message = "Not enough credits for Rally Kit";
    return false;
  }
  int fitted_count = 0;
  int64_t spent = 0;
  for (const auto& step : plan.steps) {
    if (step.already_fitted) continue;
    const auto r = fit_part_full(g, db, garage_id, step.part, step.column.c_str(), discount_percent, credits,
                                 phys_settings, pi_cfg, bands, nullptr);
    if (r.ok) {
      ++fitted_count;
      spent += r.charged;
    }
  }
  if (message) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "Fitted Rally Kit (%d parts, %lld Cr)", fitted_count, (long long)spent);
    *message = buf;
  }
  return fitted_count > 0;
}

}  // namespace garage
