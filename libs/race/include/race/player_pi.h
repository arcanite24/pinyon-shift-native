// P0-7: Resolves the raw PerformanceIndex (0..1] for the active player car so
// Race AI scales to the player's fitted garage upgrades rather than stock PI.
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"

namespace race {

// Returns the active garage car's live raw PerformanceIndex in (0, 1], or < 0
// when the driven car is a loaner / non-garage override (so RaceView uses the
// car's stock row from the database instead).
inline double resolve_player_race_pi(const garage::Car* selected_garage_car, const std::string& active_car_media,
                                     const std::map<int64_t, std::string>& baked_ids, const cars::PartsDb* db,
                                     const cars::PhysicsSettings* phys_settings, const garage::PiConfig* pi_cfg,
                                     const std::vector<garage::ClassBand>* bands) {
  if (!selected_garage_car || selected_garage_car->car_id <= 0) return -1.0;
  if (!active_car_media.empty()) {
    auto eq_ci = [](const std::string& a, const std::string& b) {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
          return false;
      return true;
    };
    if (auto it = baked_ids.find(selected_garage_car->car_id); it != baked_ids.end()) {
      if (!eq_ci(it->second, active_car_media)) return -1.0;
    } else if (db) {
      cars::CarPhysics cp;
      if (db->physics(selected_garage_car->car_id, {}, cp, nullptr) && !cp.media_name.empty() &&
          !eq_ci(cp.media_name, active_car_media)) {
        return -1.0;
      }
    }
  }
  if (db && phys_settings && pi_cfg && bands && !bands->empty()) {
    garage::PiResult res;
    if (garage::compute_pi_for_car(*db, selected_garage_car->car_id, garage::fitted(*selected_garage_car),
                                   *phys_settings, *pi_cfg, *bands, res, nullptr) &&
        res.raw_pi > 0.0) {
      return res.raw_pi;
    }
  }
  return selected_garage_car->performance_index > 0.0 ? selected_garage_car->performance_index : -1.0;
}

}  // namespace race
