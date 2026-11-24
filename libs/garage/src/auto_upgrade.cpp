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

bool is_aspiration_enum(int e) { return e == 24 || e == 25 || e == 27 || e == 28; }

// Visual body/aero parts (RearWing, Front/RearBumper, Hood, SideSkirts) are held
// back unless needed to reach the target class ceiling.
bool is_visual_aero_enum(int e) { return e == 9 || (e >= 34 && e <= 37); }

void apply_conversion_rekey(Car& car, const cars::PartsDb& db, const cars::PartRow& conv,
                            const cars::UpgradeSlot& slot) {
  car.slots[slot.column] = conv.id;
  if (conv.part_enum <= 2) {
    const int target_key = conv.part_enum == 0 ? 1 : conv.part_enum == 1 ? 2 : 3;
    for (const auto& s : cars::upgrade_slots()) {
      if (s.key != target_key) continue;
      if (is_aspiration_enum(s.part_enum) || s.part_enum == 29)
        car.slots[s.column] = -1;
      else
        car.slots.erase(s.column);
    }
    std::vector<cars::PartRow> new_opts;
    if (db.options(car.car_id, fitted(car), new_opts, nullptr)) {
      for (const auto& s : cars::upgrade_slots()) {
        if (s.key != target_key) continue;
        int64_t best_stock = -1;
        for (const auto& o : new_opts) {
          if (o.part_enum == s.part_enum && o.is_stock && (best_stock < 0 || o.id < best_stock)) {
            best_stock = o.id;
          }
        }
        if (best_stock >= 0)
          car.slots[s.column] = best_stock;
        else if (is_aspiration_enum(s.part_enum) || s.part_enum == 29)
          car.slots[s.column] = -1;
      }
    }
  } else if (is_aspiration_enum(conv.part_enum)) {
    for (int e : {24, 25, 27, 28}) {
      if (e == conv.part_enum) continue;
      if (const auto* s = slot_for_enum(e)) car.slots[s->column] = -1;
    }
    if (conv.id < 0) car.slots["Intercooler"] = -1;
  }
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
  const bool is_class_x =
      (target_class_id == int(bands.size()) - 1 && bands[size_t(target_class_id)].name == "X");
  constexpr double kOptimizePiRange = 0.003;  // GameTunableSettings.ini UpgradeWizard\OptimizePIRange
  const double stop_pi = is_class_x ? ceiling : (ceiling - kOptimizePiRange * 0.25);

  // Strict class ceiling check (never allow +1e-6 spillover into the next class!).
  auto within_ceiling = [&](const PiResult& r) -> bool {
    return r.raw_pi <= ceiling && r.class_id <= target_class_id;
  };

  // Query all available parts for this car's current engine/drivetrain/body.
  std::vector<cars::PartRow> options;
  auto reload_options = [&]() -> bool {
    options.clear();
    return db.options(working.car_id, fitted(working), options, &plan.message);
  };
  if (!reload_options()) return plan;

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
    if (p.is_stock || p.id < 0 || g.owns_part(garage_id, p.part_enum, p.id)) return 0;
    return discounted_price(p.price, discount_percent);
  };
  auto record_step = [&](const cars::PartRow& p, const char* col, bool removal) {
    const int64_t cost = removal ? 0 : part_cost(p);
    for (auto& st : plan.steps) {
      if (st.column == col) {
        st.part = p;
        st.cost = cost;
        st.removal = removal;
        return;
      }
    }
    plan.steps.push_back({p, col, cost, removal});
  };
  auto erase_family_steps = [&](int target_key) {
    plan.steps.erase(
        std::remove_if(plan.steps.begin(), plan.steps.end(), [&](const AutoUpgradeStep& st) {
          const auto* sl = slot_for_enum(st.part.part_enum);
          return sl && sl->key == target_key;
        }),
        plan.steps.end());
  };

  // Case 1: Currently above target class ceiling -> remove power parts first, then other parts.
  if (!within_ceiling(cur)) {
    auto try_remove_list = [&](const std::vector<WizardRemovalOrder>& list) {
      for (const auto& rem : list) {
        if (within_ceiling(cur)) break;
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
          if (is_aspiration_enum(rem.part_enum)) trial.slots["Intercooler"] = -1;
          replacement.part_enum = rem.part_enum;
          replacement.table = slot->table;
          replacement.id = -1;
          replacement.is_stock = true;
        }
        PiResult next = eval(trial);
        if (next.raw_pi < cur.raw_pi - 1e-6) {
          working = std::move(trial);
          cur = next;
          record_step(replacement, slot->column, true);
        }
      }
    };
    try_remove_list(power_rem);
    if (!within_ceiling(cur)) try_remove_list(parts_rem);
    // If still above ceiling, revert swapped engine/drivetrain/forced induction to stock.
    if (!within_ceiling(cur)) {
      for (int conv_enum : {24, 25, 27, 28, 0, 1}) {
        if (within_ceiling(cur)) break;
        const auto* slot = slot_for_enum(conv_enum);
        if (!slot) continue;
        const auto it = working.slots.find(slot->column);
        if (it == working.slots.end() || it->second < 0) continue;
        const auto* stk = find_stock(conv_enum);
        if (stk && it->second == stk->id) continue;
        Car trial = working;
        cars::PartRow rep;
        if (stk) {
          rep = *stk;
        } else {
          rep.part_enum = conv_enum;
          rep.table = slot->table;
          rep.id = -1;
          rep.is_stock = true;
        }
        apply_conversion_rekey(trial, db, rep, *slot);
        PiResult next = eval(trial);
        if (next.raw_pi < cur.raw_pi - 1e-6) {
          working = std::move(trial);
          cur = next;
          if (conv_enum <= 2) erase_family_steps(conv_enum == 0 ? 1 : 2);
          record_step(rep, slot->column, true);
          reload_options();
        }
      }
    }
  }

  // Helper: balanced pass over UpgradeWizardParts; `allow_visual_aero` controls
  // whether cosmetic/aero body parts (bumpers/wings/hoods/skirts) are considered.
  auto run_balanced_pass = [&](bool allow_visual_aero) {
    for (const auto& w : add_order) {
      if (cur.raw_pi >= stop_pi) break;
      if (!allow_visual_aero && is_visual_aero_enum(w.part_enum)) continue;
      const auto* slot = slot_for_enum(w.part_enum);
      if (!slot) continue;
      if (current_level(*slot) >= w.level) continue;
      const auto* cand = find_part(w.part_enum, w.level);
      if (!cand || cand->is_stock) continue;
      // Only upgrade induction slots that are currently active (or stock) in this pass;
      // aftermarket aspiration conversions are evaluated explicitly below.
      if ((is_aspiration_enum(w.part_enum) || w.part_enum == 29) && !find_stock(w.part_enum)) {
        const auto it = working.slots.find(slot->column);
        if (it == working.slots.end() || it->second < 0) continue;
      }

      Car trial = working;
      trial.slots[slot->column] = cand->id;
      PiResult next = eval(trial);
      if (within_ceiling(next) && next.raw_pi > cur.raw_pi + 1e-6) {
        working = std::move(trial);
        cur = next;
        record_step(*cand, slot->column, false);
      }
    }
  };

  auto run_finetune_pass = [&](bool allow_visual_aero) {
    if (cur.raw_pi >= (is_class_x ? ceiling : ceiling - kOptimizePiRange)) return;
    std::vector<const cars::PartRow*> extras;
    for (const auto& o : options) {
      if (o.part_enum <= 2 || o.is_stock) continue;
      if (!allow_visual_aero && is_visual_aero_enum(o.part_enum)) continue;
      if (is_aspiration_enum(o.part_enum) || o.part_enum == 29) {
        const auto* sl = slot_for_enum(o.part_enum);
        const bool active_fi = sl && working.slots.count(sl->column) && working.slots.at(sl->column) >= 0;
        if (!find_stock(o.part_enum) && !active_fi) continue;
      }
      extras.push_back(&o);
    }
    std::sort(extras.begin(), extras.end(),
              [](const cars::PartRow* a, const cars::PartRow* b) { return a->price < b->price; });
    for (const auto* cand : extras) {
      if (cur.raw_pi >= stop_pi) break;
      const auto* slot = slot_for_enum(cand->part_enum);
      if (!slot || current_level(*slot) >= cand->level) continue;
      Car trial = working;
      trial.slots[slot->column] = cand->id;
      PiResult next = eval(trial);
      if (within_ceiling(next) && next.raw_pi > cur.raw_pi + 1e-5) {
        working = std::move(trial);
        cur = next;
        record_step(*cand, slot->column, false);
      }
    }
  };

  auto try_aspiration_conversion = [&]() -> bool {
    if (cur.raw_pi >= (is_class_x ? ceiling : ceiling - kOptimizePiRange)) return false;
    bool has_active_fi = false;
    for (int e : {24, 25, 27, 28}) {
      const auto* sl = slot_for_enum(e);
      if (sl && working.slots.count(sl->column) && working.slots.at(sl->column) >= 0) has_active_fi = true;
    }
    if (has_active_fi) return false;
    cars::PartRow best_conv;
    const cars::UpgradeSlot* best_slot = nullptr;
    PiResult best_pi = cur;
    Car best_car = working;
    bool found = false;
    for (const auto& o : options) {
      if (!is_aspiration_enum(o.part_enum) || o.is_stock) continue;
      const auto* sl = slot_for_enum(o.part_enum);
      if (!sl) continue;
      Car trial = working;
      apply_conversion_rekey(trial, db, o, *sl);
      PiResult next = eval(trial);
      if (within_ceiling(next) && next.raw_pi > best_pi.raw_pi + 1e-5) {
        best_conv = o;
        best_slot = sl;
        best_pi = next;
        best_car = std::move(trial);
        found = true;
      }
    }
    if (!found || !best_slot) return false;
    working = std::move(best_car);
    cur = best_pi;
    record_step(best_conv, best_slot->column, false);
    reload_options();
    // Also upgrade the newly installed induction / intercooler if room remains.
    run_balanced_pass(false);
    return true;
  };

  auto try_swap_conversion = [&](int swap_enum) -> bool {
    if (cur.raw_pi >= (is_class_x ? ceiling : ceiling - kOptimizePiRange)) return false;
    const auto* sl = slot_for_enum(swap_enum);
    if (!sl) return false;
    const int target_key = (swap_enum == 0) ? 1 : 2;
    cars::PartRow best_swap;
    PiResult best_score_pi = cur;
    PiResult best_base_pi = cur;
    Car best_car = working;
    bool found = false;
    for (const auto& o : options) {
      if (o.part_enum != swap_enum || o.is_stock) continue;
      const auto sit = working.slots.find(sl->column);
      if (sit != working.slots.end() && sit->second == o.id) continue;
      Car trial = working;
      apply_conversion_rekey(trial, db, o, *sl);
      PiResult base_next = eval(trial);
      if (!within_ceiling(base_next)) continue;

      // For Engine swaps, evaluate peak potential with the donor engine's own upgrades
      // so a stronger donor engine is not rejected just because its stock sub-parts
      // start slightly below the already-upgraded original engine.
      PiResult score_next = base_next;
      if (swap_enum == 0 && base_next.raw_pi <= cur.raw_pi + 1e-5) {
        Car peak = trial;
        std::vector<cars::PartRow> swap_opts;
        if (db.options(peak.car_id, fitted(peak), swap_opts, nullptr)) {
          for (int pe : {19, 18, 12, 14, 15, 16, 24, 25, 27, 28, 29}) {
            const auto* sub_sl = slot_for_enum(pe);
            if (!sub_sl) continue;
            const cars::PartRow* top = nullptr;
            for (const auto& so : swap_opts) {
              if (so.part_enum == pe && !so.is_stock && (!top || so.level > top->level)) top = &so;
            }
            if (!top) continue;
            Car step_try = peak;
            if (is_aspiration_enum(pe))
              apply_conversion_rekey(step_try, db, *top, *sub_sl);
            else
              step_try.slots[sub_sl->column] = top->id;
            PiResult step_pi = eval(step_try);
            if (within_ceiling(step_pi) && step_pi.raw_pi > score_next.raw_pi + 1e-5) {
              peak = std::move(step_try);
              score_next = step_pi;
            }
          }
        }
      }

      if (score_next.raw_pi > best_score_pi.raw_pi + 1e-5) {
        best_swap = o;
        best_score_pi = score_next;
        best_base_pi = base_next;
        best_car = std::move(trial);
        found = true;
      }
    }
    if (!found) return false;
    working = std::move(best_car);
    cur = best_base_pi;
    erase_family_steps(target_key);
    record_step(best_swap, sl->column, false);
    reload_options();
    run_balanced_pass(false);
    try_aspiration_conversion();
    return true;
  };

  // Pass 1: Mechanical upgrades (no visual bumpers/wings forced).
  run_balanced_pass(false);

  // Pass 2: If still below target class ceiling, try forced-induction conversion,
  // drivetrain swap, and engine swap (with full dependent sub-part re-keying).
  if (cur.raw_pi < (is_class_x ? ceiling : ceiling - kOptimizePiRange)) {
    try_aspiration_conversion();
  }
  if (cur.raw_pi < (is_class_x ? ceiling : ceiling - kOptimizePiRange)) {
    try_swap_conversion(1);  // Drivetrain swap (e.g. AWD)
  }
  if (cur.raw_pi < (is_class_x ? ceiling : ceiling - kOptimizePiRange)) {
    try_swap_conversion(0);  // Engine swap
  }

  // Pass 3: Fine-tune remaining mechanical upgrades.
  run_finetune_pass(false);

  // Pass 4: Only consider visual aero bumpers/wings if STILL needed to reach target class!
  if (cur.raw_pi < (is_class_x ? ceiling : ceiling - kOptimizePiRange)) {
    run_balanced_pass(true);
    run_finetune_pass(true);
  }

  plan.total_cost = 0;
  for (const auto& st : plan.steps) plan.total_cost += st.cost;
  plan.result = cur;
  plan.ok = within_ceiling(cur) && !plan.steps.empty();
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
      if (is_aspiration_enum(step.part.part_enum)) car->slots["Intercooler"] = -1;
    } else {
      FitResult r = fit_part_full(g, db, garage_id, step.part, step.column.c_str(), discount_percent, credits);
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
