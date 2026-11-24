#include "cars/visual_setup.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "cars/util.h"

namespace cars {
namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

std::string col_text(sqlite3_stmt* st, int col) {
  const auto* p = sqlite3_column_text(st, col);
  return p ? reinterpret_cast<const char*>(p) : "";
}

bool is_chrome_wheel(const std::string& wheel_color, const std::string& actual_color, const std::string& media) {
  const std::string wc = lower(wheel_color);
  const std::string ac = lower(actual_color);
  const std::string mn = lower(media);
  return wc.find("chrome") != std::string::npos || ac.find("chrome") != std::string::npos ||
         mn.find("chrome") != std::string::npos;
}

void parse_parts_string(const std::string& raw, std::set<std::string>& shown, std::set<std::string>& removed) {
  size_t at = 0;
  while (at <= raw.size()) {
    size_t end = raw.find(',', at);
    if (end == std::string::npos) end = raw.size();
    std::string tok = raw.substr(at, end - at);
    while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
    while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
    tok = lower(tok);
    if (!tok.empty()) {
      if (tok[0] == '-') removed.insert(tok.substr(1));
      else shown.insert(tok);
    }
    at = end + 1;
  }
}

}  // namespace

std::string per_car_tyre_media(std::string_view car_media) {
  const std::string m = lower(std::string(car_media));
  // Six models ship their own TireA<MediaName> set (FORMAT-CARS, Wheels).
  if (m == "for_svtraptor_12" || m == "for_svtraptor_12_lce" || m == "for_svtraptor_12_halo")
    return "tireafor_svtraptor_12";
  if (m == "hum_h1alpha_06") return "tireahum_h1alpha_06";
  if (m == "jee_wrangler_12" || m == "jee_wranglerrubicon_12") return "tireajee_wrangler_12";
  if (m == "toy_fjcruiser_12") return "tireatoy_fjcruiser_12";
  return {};
}

bool is_own_tyre_car(std::string_view car_media) { return !per_car_tyre_media(car_media).empty(); }

const char* brake_type_stem(int type_id) {
  switch (type_id) {
    case 1: return "drum";
    case 2: return "rotor_steel";
    case 3: return "rotor_drilled";
    case 4: return "rotor_slotted";
    case 5: return "rotor_carbon";
    default: return "rotor_steel";
  }
}

bool query_aftermarket_wheels(const PartsDb& db, std::vector<WheelRow>& out, std::string* error, bool include_stock) {
  out.clear();
  std::map<int64_t, std::string> mfgs;
  db.each("SELECT Id, DisplayName FROM List_PartManufacturer", {},
          [&](sqlite3_stmt* st) { mfgs[sqlite3_column_int64(st, 0)] = col_text(st, 1); }, nullptr);

  const std::string sql =
      std::string(
          "SELECT ID, MediaName, DisplayName, PartManufacturerID, Price, Mass, BrakeDrawOffset, IsStock, "
          "WheelColor, ActualColor, RGB FROM List_Wheels WHERE MediaName IS NOT NULL AND MediaName != '' ") +
      (include_stock ? "" : "AND IsStock = 0 ") + "ORDER BY PartManufacturerID, DisplayName, ID";

  return db.each(
      sql, {},
      [&](sqlite3_stmt* st) {
        WheelRow r;
        r.id = sqlite3_column_int64(st, 0);
        r.media_name = col_text(st, 1);
        r.display_name = col_text(st, 2);
        r.manufacturer_id = sqlite3_column_int64(st, 3);
        if (auto it = mfgs.find(r.manufacturer_id); it != mfgs.end()) r.manufacturer_key = it->second;
        r.price = sqlite3_column_int64(st, 4);
        r.mass = sqlite3_column_double(st, 5);
        r.brake_draw_offset = sqlite3_column_double(st, 6);
        r.is_stock = sqlite3_column_int64(st, 7) != 0;
        r.wheel_color = col_text(st, 8);
        r.actual_color = col_text(st, 9);
        r.rgb = uint32_t(sqlite3_column_int64(st, 10));
        r.is_chrome = is_chrome_wheel(r.wheel_color, r.actual_color, r.media_name);
        out.push_back(std::move(r));
      },
      error);
}

bool query_wheel_by_id(const PartsDb& db, int64_t wheel_id, WheelRow& out, std::string* error) {
  out = WheelRow{};
  bool found = false;
  const bool ok = db.each(
      "SELECT ID, MediaName, DisplayName, PartManufacturerID, Price, Mass, BrakeDrawOffset, IsStock, "
      "WheelColor, ActualColor, RGB FROM List_Wheels WHERE ID = ?",
      {std::to_string(wheel_id)},
      [&](sqlite3_stmt* st) {
        out.id = sqlite3_column_int64(st, 0);
        out.media_name = col_text(st, 1);
        out.display_name = col_text(st, 2);
        out.manufacturer_id = sqlite3_column_int64(st, 3);
        out.price = sqlite3_column_int64(st, 4);
        out.mass = sqlite3_column_double(st, 5);
        out.brake_draw_offset = sqlite3_column_double(st, 6);
        out.is_stock = sqlite3_column_int64(st, 7) != 0;
        out.wheel_color = col_text(st, 8);
        out.actual_color = col_text(st, 9);
        out.rgb = uint32_t(sqlite3_column_int64(st, 10));
        out.is_chrome = is_chrome_wheel(out.wheel_color, out.actual_color, out.media_name);
        found = true;
      },
      error);
  return ok && found;
}

bool query_track_offsets(const PartsDb& db, int64_t car_id, CarTrackOffsetRow& out, std::string* error) {
  out = CarTrackOffsetRow{};
  const bool ok = db.each(
      "SELECT FrontOffset, RearOffset, UpgradeFrontOffset, UpgradeRearOffset FROM CarTrackOffsets WHERE Id = ?",
      {std::to_string(car_id)},
      [&](sqlite3_stmt* st) {
        out.found = true;
        out.front_offset = sqlite3_column_double(st, 0);
        out.rear_offset = sqlite3_column_double(st, 1);
        out.upgrade_front_offset = sqlite3_column_double(st, 2);
        out.upgrade_rear_offset = sqlite3_column_double(st, 3);
      },
      error);
  return ok;
}

bool resolve_visual_setup(const PartsDb& db, int64_t car_id, const FittedParts& fit, VisualSetup& out,
                          std::string* error, float tune_front_rh_norm, float tune_rear_rh_norm,
                          float tune_front_camber_deg, float tune_rear_camber_deg) {
  out = VisualSetup{};
  CarPhysics phys;
  if (!db.physics(car_id, fit, phys, error)) return false;

  out.active = true;
  out.tyre_media_override = per_car_tyre_media(phys.media_name);

  // 1. Visual body & exhaust parts from List_PartsStrings.
  // Query the database directly because some tables (RearWing, FrontBumper,
  // RearBumper, SideSkirt, Hood) store PartsStringId as SQLITE_TEXT while
  // others store it as SQLITE_INTEGER.
  std::map<int64_t, std::string> part_strings;
  db.each("SELECT Id, PartsString FROM List_PartsStrings", {},
          [&](sqlite3_stmt* st) { part_strings[sqlite3_column_int64(st, 0)] = col_text(st, 1); }, nullptr);

  const int64_t body_id = int64_t(std::llround(phys.get("List_UpgradeCarBody.CarBodyID", double(car_id * 1000))));
  const int64_t engine_id = int64_t(std::llround(phys.get("List_UpgradeEngine.EngineID", 0.0)));

  std::set<std::string> shown, removed;
  struct PartStrSlot {
    const char* table;
    const char* key_col;
    const char* ps_col;
    int key_kind;  // 0=car_id, 1=engine_id, 3=body_id
    bool is_exhaust;
  };
  static constexpr PartStrSlot kSlots[] = {
      {"List_UpgradeCarBodyFrontBumper", "CarBodyID", "PartsStringId", 3, false},
      {"List_UpgradeCarBodyRearBumper", "CarBodyID", "PartsStringId", 3, false},
      {"List_UpgradeCarBodySideSkirt", "CarBodyID", "PartsStringId", 3, false},
      {"List_UpgradeCarBodyHood", "CarBodyID", "PartsStringId", 3, false},
      {"List_UpgradeRearWing", "Ordinal", "PartsStringId", 0, false},
      {"List_UpgradeCarBodyChassisStiffness", "CarbodyId", "PartsStringID", 3, false},
      {"List_UpgradeCarBodyWeight", "CarBodyId", "PartsStringID", 3, false},
      {"List_UpgradeEngineExhaust", "EngineID", "PartsStringID", 1, true},
  };
  for (const auto& sl : kSlots) {
    int64_t ps_id = 0;
    auto fit_it = fit.find(sl.table);
    if (fit_it != fit.end() && fit_it->second >= 0) {
      db.each(std::string("SELECT CAST(") + sl.ps_col + " AS INTEGER) FROM " + sl.table + " WHERE Id = ? LIMIT 1",
              {std::to_string(fit_it->second)},
              [&](sqlite3_stmt* st) { ps_id = sqlite3_column_int64(st, 0); }, nullptr);
    } else {
      const int64_t key_val = sl.key_kind == 0 ? car_id : sl.key_kind == 1 ? engine_id : body_id;
      db.each(std::string("SELECT CAST(") + sl.ps_col + " AS INTEGER) FROM " + sl.table + " WHERE " + sl.key_col +
                  " = ? AND IsStock = 1 ORDER BY Id LIMIT 1",
              {std::to_string(key_val)},
              [&](sqlite3_stmt* st) { ps_id = sqlite3_column_int64(st, 0); }, nullptr);
    }
    if (ps_id <= 0) continue;
    auto it = part_strings.find(ps_id);
    if (it == part_strings.end() || it->second.empty()) continue;
    if (sl.is_exhaust) {
      std::set<std::string> ex_shown, ex_rem;
      parse_parts_string(it->second, ex_shown, ex_rem);
      if (!ex_shown.empty()) {
        out.has_exhaust_override = true;
        out.exhaust_part = *ex_shown.begin();
        shown.insert(ex_shown.begin(), ex_shown.end());
      }
      removed.insert(ex_rem.begin(), ex_rem.end());
    } else {
      parse_parts_string(it->second, shown, removed);
    }
  }
  out.shown_parts.assign(shown.begin(), shown.end());
  out.removed_parts.assign(removed.begin(), removed.end());

  // 2. Upgraded tyre widths & rim diameters.
  const int64_t fw = int64_t(std::llround(phys.get("Data_Car.FrontTireWidthMM", 0.0)));
  const int64_t rw = int64_t(std::llround(phys.get("Data_Car.RearTireWidthMM", 0.0)));
  const int64_t fr = int64_t(std::llround(phys.get("Data_Car.FrontWheelDiameterIN", 0.0)));
  const int64_t rr = int64_t(std::llround(phys.get("Data_Car.RearWheelDiameterIN", 0.0)));
  if (fw > 0) out.tyre_width_mm[0] = fw;
  if (rw > 0) out.tyre_width_mm[1] = rw;
  if (fr > 0) out.rim_inches[0] = fr;
  if (rr > 0) out.rim_inches[1] = rr;

  // 3. Aftermarket wheel (List_Wheels) + BrakeDrawOffset.
  int64_t wheel_id = int64_t(std::llround(phys.get("Data_Car.StockWheelID", 0.0)));
  if (auto it = fit.find("List_Wheels"); it != fit.end() && it->second > 0) wheel_id = it->second;
  if (wheel_id > 0) {
    WheelRow w;
    if (query_wheel_by_id(db, wheel_id, w, nullptr)) {
      out.wheel_id = w.id;
      out.wheel_media = lower(w.media_name);
      out.brake_draw_offset = float(w.brake_draw_offset);
      out.wheel_is_chrome = w.is_chrome;
      out.wheel_is_stock = w.is_stock;
    }
  }
  out.tire_compound_level = std::clamp(int(std::llround(phys.get("List_UpgradeCarBodyTireCompound.Level", 0.0))), 0, 4);

  // 4. Suspension ride height & camber (fitted SpringDamperPhysics + optional tuning).
  auto eval_rh = [&](const std::string& prefix, const std::string& stock_col, float norm) -> float {
    const double def_rh = phys.get(prefix + ".DefRideHeight",
                                   phys.get(prefix + ".RideHeightDefault", phys.get(stock_col, -1.0)));
    if (norm >= 0.0f) {
      const double lo = phys.get(prefix + ".MinRideHeight", def_rh);
      const double hi = phys.get(prefix + ".MaxRideHeight", def_rh);
      if (lo >= 0.0 && hi >= lo) return float(lo + (hi - lo) * std::clamp(double(norm), 0.0, 1.0));
    }
    return float(def_rh);
  };
  out.ride_height_m[0] = eval_rh("FrontSpringDamper", "Data_Car.FrontStockRideHeight", tune_front_rh_norm);
  out.ride_height_m[1] = eval_rh("RearSpringDamper", "Data_Car.RearStockRideHeight", tune_rear_rh_norm);

  const double cam_f = tune_front_camber_deg > -90.0f ? double(tune_front_camber_deg)
                                                      : phys.get("FrontSpringDamper.DefCamber", 0.0);
  const double cam_r = tune_rear_camber_deg > -90.0f ? double(tune_rear_camber_deg)
                                                     : phys.get("RearSpringDamper.DefCamber", 0.0);
  out.camber_rad[0] = float(cam_f) * kDegToRad;
  out.camber_rad[1] = float(cam_r) * kDegToRad;

  // 5. CarTrackOffsets: base stock FrontOffset/RearOffset is already baked into
  // CarInfo::track_outer (bake.cpp), so only store the additional upgrade delta here
  // when tyre width or rim size is upgraded.
  CarTrackOffsetRow toff;
  if (query_track_offsets(db, car_id, toff, nullptr) && toff.found) {
    const bool upg_f = phys.get("List_UpgradeCarBodyTireWidthFront.Level", 0.0) > 0.0 ||
                       phys.get("List_UpgradeRimSizeFront.Level", 0.0) > 0.0;
    const bool upg_r = phys.get("List_UpgradeCarBodyTireWidthRear.Level", 0.0) > 0.0 ||
                       phys.get("List_UpgradeRimSizeRear.Level", 0.0) > 0.0;
    out.track_offset_m[0] = float(upg_f ? toff.upgrade_front_offset : 0.0);
    out.track_offset_m[1] = float(upg_r ? toff.upgrade_rear_offset : 0.0);
  }

  // 6. Upgraded brakes + caliper angles.
  const int brake_lvl = int(std::llround(phys.get("List_UpgradeBrakes.Level", 0.0)));
  out.brakes[0] = {int(std::llround(phys.get("List_UpgradeBrakes.FrontBrakeSize", 0.0))),
                   int(std::llround(phys.get("List_UpgradeBrakes.FrontBrakeTypeID", 2.0))),
                   int(std::llround(phys.get("List_UpgradeBrakes.FrontCaliperPistons", 2.0))),
                   brake_lvl};
  out.brakes[1] = {int(std::llround(phys.get("List_UpgradeBrakes.RearBrakeSize", 0.0))),
                   int(std::llround(phys.get("List_UpgradeBrakes.RearBrakeTypeID", 2.0))),
                   int(std::llround(phys.get("List_UpgradeBrakes.RearCaliperPistons", 2.0))),
                   brake_lvl};
  out.caliper_angle_deg[0] = float(phys.get("Data_Car.CaliperAngleFront", 0.0));
  out.caliper_angle_deg[1] = float(phys.get("Data_Car.CaliperAngleRear", 0.0));
  out.has_caliper_angle_override = true;

  return true;
}

}  // namespace cars
