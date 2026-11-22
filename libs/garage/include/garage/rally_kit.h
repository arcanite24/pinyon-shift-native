// Horizon Rally upgrade kit inspection, planning and one-click conversion
// (SPEC-V4-CONTENT: restriction flag 0x80, Rally tyre compound 9 / level 5,
// Rally suspension level 4, Rally transmission level 4, OptionalRallyUpgrades).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "garage/garage.h"

namespace garage {

struct RallyKitStatus {
  bool has_rally_tyres = false;
  bool has_rally_suspension = false;
  bool has_rally_transmission = false;
  bool has_optional_aero = false;      // car has OptionalRallyUpgrades rows
  int optional_aero_fitted = 0;
  int optional_aero_total = 0;
  bool can_convert = false;            // car has Rally tyre row in DB
  bool is_rally_upgraded = false;      // satisfies v4 restriction bit 0x80
  bool is_full_kit = false;            // tyres + suspension + transmission all fitted
};

struct RallyKitStep {
  cars::PartRow part;
  std::string column;
  std::string label;
  int64_t cost = 0;
  bool already_fitted = false;
  bool already_owned = false;
  bool optional_aero = false;
};

struct RallyKitPlan {
  bool ok = false;
  bool already_complete = false;
  int64_t total_cost = 0;
  double start_pi = 0.0;
  int start_class_id = 0;
  int start_shown_pi = 0;
  PiResult result;
  std::vector<RallyKitStep> steps;
  std::string message;
};

// Inspects whether `car` has Rally tyres (compound 9 / level 5), Rally
// suspension (level 4), Rally transmission (level 4), and optional Rally aero.
RallyKitStatus inspect_rally_kit(const Car& car, const cars::PartsDb& db);

// Plans a Rally Kit conversion (Rally tyres + Rally suspension + Rally
// transmission + optional Rally aero/light pods when `include_optional_aero`).
RallyKitPlan plan_rally_kit(const Garage& g, const cars::PartsDb& db, int64_t garage_id, int discount_percent,
                            bool include_optional_aero = true, const cars::PhysicsSettings* phys_settings = nullptr,
                            const PiConfig* pi_cfg = nullptr, const std::vector<ClassBand>* bands = nullptr);

// Applies a previously computed RallyKitPlan if `credits >= plan.total_cost`.
bool apply_rally_kit(Garage& g, const cars::PartsDb& db, int64_t garage_id, const RallyKitPlan& plan,
                     int discount_percent, int64_t& credits, const cars::PhysicsSettings* phys_settings = nullptr,
                     const PiConfig* pi_cfg = nullptr, const std::vector<ClassBand>* bands = nullptr,
                     std::string* message = nullptr);

// True if `part` is a v4 Rally part (tyre level 5, suspension level 4, or
// transmission level 4).
bool is_rally_part(int part_enum, int level);

}  // namespace garage
