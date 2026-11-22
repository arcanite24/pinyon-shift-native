#include "garage/garage.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace garage {
namespace {

struct WizardPartOrder {
  int part_enum = 0;
  int level = 0;
  int sort_order = 0;
};

struct WizardRemovalOrder {
  int part_enum = 0;
  int order = 0;
};

const cars::UpgradeSlot* slot_for_enum(int part_enum) {
  for (const auto& s : cars::upgrade_slots())
    if (s.part_enum == part_enum) return &s;
  return nullptr;
}

}  // namespace

AutoUpgradePlan plan_auto_upgrade(const Garage& g, const cars::PartsDb& db, int64_t garage_id, int target_class_id,
                                  int discount_percent, const cars::PhysicsSettings& phys_settings,
                                  const PiConfig& pi_cfg, const std::vector<ClassBand>& bands) {
  AutoUpgradePlan plan;
  plan.target_class_id = target_class_id;
  const Car* orig = g.find(garage_id);
  if (!orig || !db.is_open() || bands.empty()) {
    plan.message = "car or database unavailable";
    return plan;
  }
  if (target_class_id < 0 || target_class_id >= int(bands.size())) {
    plan.message = "invalid target class";
    return plan;
  }

  // Load UpgradeWizard tables from merged DB.
  std::vector<WizardPartOrder> add_order;
  db.each("SELECT PartEnum, PartLevel, SortBalanced FROM UpgradeWizardParts ORDER BY SortBalanced, PartEnum, PartLevel",
          {},
          [&](sqlite3_stmt* st) {
            add_order.push_back({sqlite3_column_int(st, 0), sqlite3_column_int(st, 1), sqlite3_column_int(st, 2)});
          },
          nullptr);
  std::vector<WizardRemovalOrder> power_rem, parts_rem;
  db.each("SELECT PartEnum, RemoveOrder FROM UpgradeWizardPowerRemoval ORDER BY RemoveOrder, PartEnum", {},
          [&](sqlite3_stmt* st) { power_rem.push_back({sqlite3_column_int(st, 0), sqlite3_column_int(st, 1)}); },
          nullptr);
  db.each("SELECT PartEnum, SortBalanced FROM UpgradeWizardPartsRemoval ORDER BY SortBalanced, PartEnum", {},
          [&](sqlite3_stmt* st) { parts_rem.push_back({sqlite3_column_int(st, 0), sqlite3_column_int(st, 1)}); },
          nullptr);

  // Compute stock physics once so every trial step is fast.
  cars::CarPhysics stock_phys;
  if (!db.physics(orig->car_id, {}, stock_phys, &plan.message)) return plan;

  Car working = *orig;
  auto eval = [&](const Car& c) -> PiResult {
    cars::CarPhysics p;
    if (!db.physics(c.car_id, fitted(c), p, nullptr)) return PiResult{};
    return compute_pi(p, phys_settings, pi_cfg, bands, &stock_phys);
  };

  PiResult cur = eval(working);
  plan.start_pi = cur.raw_pi;
  plan.start_class_id = cur.class_id;
  plan.start_shown_pi = cur.shown_pi;
  plan.result = cur;

  const double ceiling = bands[size_t(target_class_id)].raw_max;
  constexpr double kOptimizePiRange = 0.003;  // GameTunableSettings.ini UpgradeWizard\OptimizePIRange

  // Query all available parts for this car's current engine/drivetrain/body.
  std::vector<cars::PartRow> options;
  if (!db.options(working.car_id, fitted(working), options, &plan.message)) return plan;

  auto find_stock = [&](int part_enum) -> const cars::PartRow* {
    const cars::PartRow* best = nullptr;
    for (const auto& o : options)
      if (o.part_enum == part_enum && o.is_stock && (!best || o.id < best->id)) best = &o;
    return best;
  };
  auto find_part = [&](int part_enum, int level) -> const cars::PartRow* {
    const cars::PartRow* best = nullptr;
    for (const auto& o : options) {
      if (o.part_enum != part_enum || o.level != level) continue;
      if (!best || o.price < best->price) best = &o;
    }
    return best;
  };
  auto current_level = [&](const cars::UpgradeSlot& s) -> int {
    const auto it = working.slots.find(s.column);
    if (it == working.slots.end() || it->second < 0) return -1;
    for (const auto& o : options)
      if (o.part_enum == s.part_enum && o.id == it->second) return o.level;
    return 0;
  };
  auto part_cost = [&](const cars::PartRow& p) -> int64_t {
    if (p.is_stock || g.owns_part(garage_id, p.part_enum, p.id)) return 0;
    return discounted_price(p.price, discount_percent);
  };

  // Case 1: Currently above target class ceiling -> remove power parts first, then other parts.
  if (cur.raw_pi > ceiling) {
    auto try_remove_list = [&](const std::vector<WizardRemovalOrder>& list) {
      for (const auto& rem : list) {
        if (cur.raw_pi <= ceiling) break;
        const auto* slot = slot_for_enum(rem.part_enum);
        if (!slot) continue;
        const auto* stk = find_stock(rem.part_enum);
        const auto it = working.slots.find(slot->column);
        if (it == working.slots.end() || it->second < 0) continue;
        if (stk && it->second == stk->id) continue;

        Car trial = working;
        cars::PartRow replacement;
        if (stk) {
          trial.slots[slot->column] = stk->id;
          replacement = *stk;
        } else {
          trial.slots[slot->column] = -1;
          replacement.part_enum = rem.part_enum;
          replacement.table = slot->table;
          replacement.id = -1;
          replacement.is_stock = true;
        }
        PiResult next = eval(trial);
        if (next.raw_pi < cur.raw_pi - 1e-6) {
          working = std::move(trial);
          cur = next;
          plan.steps.push_back({replacement, slot->column, 0, true});
        }
      }
    };
    try_remove_list(power_rem);
    if (cur.raw_pi > ceiling) try_remove_list(parts_rem);
  }

  // Case 2: Below or at ceiling -> add parts in UpgradeWizardParts balanced order
  // while staying at or under the class ceiling (aiming within OptimizePIRange).
  for (const auto& w : add_order) {
    if (cur.raw_pi >= ceiling - kOptimizePiRange * 0.25) break;
    const auto* slot = slot_for_enum(w.part_enum);
    if (!slot) continue;
    if (current_level(*slot) >= w.level) continue;
    const auto* cand = find_part(w.part_enum, w.level);
    if (!cand || cand->is_stock) continue;
    // Skip aspiration conversions on cars that do not have a stock row for that induction unless already forced.
    if ((w.part_enum == 24 || w.part_enum == 25 || w.part_enum == 27 || w.part_enum == 28 || w.part_enum == 29) &&
        !find_stock(w.part_enum)) {
      const auto it = working.slots.find(slot->column);
      if (it == working.slots.end() || it->second < 0) continue;
    }

    Car trial = working;
    trial.slots[slot->column] = cand->id;
    PiResult next = eval(trial);
    if (next.raw_pi <= ceiling + 1e-6 && next.raw_pi > cur.raw_pi + 1e-6) {
      working = std::move(trial);
      cur = next;
      // Replace any earlier step for the same column so we only charge/fit the final chosen level per slot.
      bool merged = false;
      for (auto& st : plan.steps) {
        if (st.column == slot->column) {
          st.part = *cand;
          st.cost = part_cost(*cand);
          st.removal = false;
          merged = true;
          break;
        }
      }
      if (!merged) plan.steps.push_back({*cand, slot->column, part_cost(*cand), false});
    }
  }

  // Fine-tune pass: try remaining small upgrades (cheapest first) to close gap within OptimizePIRange.
  if (cur.raw_pi < ceiling - kOptimizePiRange) {
    std::vector<const cars::PartRow*> extras;
    for (const auto& o : options) {
      if (o.part_enum <= 2 || o.is_stock) continue;
      if (o.part_enum == 24 || o.part_enum == 25 || o.part_enum == 27 || o.part_enum == 28 || o.part_enum == 29) {
        if (!find_stock(o.part_enum)) continue;
      }
      extras.push_back(&o);
    }
    std::sort(extras.begin(), extras.end(),
              [](const cars::PartRow* a, const cars::PartRow* b) { return a->price < b->price; });
    for (const auto* cand : extras) {
      if (cur.raw_pi >= ceiling - kOptimizePiRange * 0.25) break;
      const auto* slot = slot_for_enum(cand->part_enum);
      if (!slot || current_level(*slot) >= cand->level) continue;
      Car trial = working;
      trial.slots[slot->column] = cand->id;
      PiResult next = eval(trial);
      if (next.raw_pi <= ceiling + 1e-6 && next.raw_pi > cur.raw_pi + 1e-5) {
        working = std::move(trial);
        cur = next;
        bool merged = false;
        for (auto& st : plan.steps) {
          if (st.column == slot->column) {
            st.part = *cand;
            st.cost = part_cost(*cand);
            st.removal = false;
            merged = true;
            break;
          }
        }
        if (!merged) plan.steps.push_back({*cand, slot->column, part_cost(*cand), false});
      }
    }
  }

  plan.total_cost = 0;
  for (const auto& st : plan.steps) plan.total_cost += st.cost;
  plan.result = cur;
  plan.ok = (cur.raw_pi <= ceiling + 1e-6) && !plan.steps.empty();
  if (plan.steps.empty()) {
    plan.message = cur.class_id == target_class_id ? "car already fits target class" : "cannot reach target class";
  } else if (!plan.ok) {
    plan.message = "cannot lower car into target class";
  }
  return plan;
}

bool apply_auto_upgrade(Garage& g, const cars::PartsDb& db, int64_t garage_id, const AutoUpgradePlan& plan,
                        int discount_percent, int64_t& credits, const cars::PhysicsSettings& phys_settings,
                        const PiConfig& pi_cfg, const std::vector<ClassBand>& bands, std::string* message) {
  if (!plan.ok) {
    if (message) *message = plan.message.empty() ? "invalid auto-upgrade plan" : plan.message;
    return false;
  }
  if (credits < plan.total_cost) {
    if (message) *message = "not enough CR for auto upgrade";
    return false;
  }
  Car* car = g.find(garage_id);
  if (!car) {
    if (message) *message = "car not found";
    return false;
  }
  for (const auto& step : plan.steps) {
    if (step.removal && step.part.id < 0) {
      car->slots[step.column] = -1;
    } else {
      FitResult r = fit_part(g, garage_id, step.part, step.column.c_str(), discount_percent, credits);
      if (!r.ok) {
        if (message) *message = "failed fitting " + step.column;
        return false;
      }
    }
  }
  refresh_car_pi(*car, db, phys_settings, pi_cfg, bands);
  if (message) {
    const std::string cls = bands.empty() ? "?" : bands[size_t(car->class_id)].name;
    *message = "auto upgraded to " + cls + " " + std::to_string(shown_index(car->performance_index, bands));
  }
  return true;
}

}  // namespace garage
