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

float compute_tire_outer_radius_m(int64_t stock_rim_in, int64_t stock_width_mm, int64_t stock_aspect,
                                  int64_t fitted_rim_in, int64_t fitted_width_mm) {
  (void)fitted_width_mm;
  const float s_rim_in = stock_rim_in > 0 ? float(stock_rim_in) : 14.0f;
  const float s_rim_r = s_rim_in * 0.0254f * 0.5f;
  const float s_width_m = stock_width_mm > 0 ? float(stock_width_mm) / 1000.0f : 0.2f;
  const float s_aspect = stock_aspect > 0 ? float(stock_aspect) / 100.0f : 0.5f;
  const float stock_outer = s_rim_r + s_width_m * s_aspect;

  const float f_rim_in = fitted_rim_in > 0 ? float(fitted_rim_in) : s_rim_in;
  const float f_rim_r = f_rim_in * 0.0254f * 0.5f;
  // Preserve stock rolling outer radius as rim diameter or tyre width increases,
  // while keeping at least a 25 mm low-profile sidewall so the rim never protrudes.
  constexpr float kMinSidewallM = 0.025f;
  return std::max(stock_outer, f_rim_r + kMinSidewallM);
}

char tire_compound_texture_letter(int compound_level) {
  if (compound_level < 2) return 'a';
  if (compound_level == 2) return 'b';
  return 'c';
}

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

  std::set<int64_t> wheel_exceptions;
  db.each("SELECT WheelID FROM WheelExceptions", {},
          [&](sqlite3_stmt* st) { wheel_exceptions.insert(sqlite3_column_int64(st, 0)); }, nullptr);

  std::map<int64_t, double> wheel_norm_offsets;
  db.each("SELECT Id, NormOffset FROM WheelNormOffsets", {},
          [&](sqlite3_stmt* st) { wheel_norm_offsets[sqlite3_column_int64(st, 0)] = sqlite3_column_double(st, 1); },
          nullptr);

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
        r.is_exception = wheel_exceptions.count(r.id) != 0;
        r.is_paintable = !r.is_stock && !r.is_chrome && !r.is_exception;
        if (auto it = wheel_norm_offsets.find(r.id); it != wheel_norm_offsets.end()) r.norm_offset = it->second;
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
  if (ok && found) {
    bool is_exc = false;
    query_wheel_is_exception(db, wheel_id, is_exc, nullptr);
    out.is_exception = is_exc;
    out.is_paintable = !out.is_stock && !out.is_chrome && !out.is_exception;
    float norm_off = 0.0f;
    if (query_wheel_norm_offset(db, wheel_id, norm_off, nullptr)) {
      out.norm_offset = double(norm_off);
    }
  }
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

bool query_wheel_norm_offset(const PartsDb& db, int64_t wheel_id, float& out_offset, std::string* error) {
  out_offset = 0.0f;
  if (wheel_id <= 0) return true;
  return db.each(
      "SELECT NormOffset FROM WheelNormOffsets WHERE Id = ? LIMIT 1", {std::to_string(wheel_id)},
      [&](sqlite3_stmt* st) { out_offset = float(sqlite3_column_double(st, 0)); }, error);
}

bool query_wheel_is_exception(const PartsDb& db, int64_t wheel_id, bool& out_is_exception, std::string* error) {
  out_is_exception = false;
  if (wheel_id <= 0) return true;
  return db.each(
      "SELECT 1 FROM WheelExceptions WHERE WheelID = ? LIMIT 1", {std::to_string(wheel_id)},
      [&](sqlite3_stmt*) { out_is_exception = true; }, error);
}

bool is_wheel_paintable(const PartsDb& db, int64_t wheel_id, std::string* error) {
  if (wheel_id <= 0) return false;
  WheelRow w;
  if (!query_wheel_by_id(db, wheel_id, w, error)) return false;
  return w.is_paintable;
}

bool car_paint_exceptions(const PartsDb& db, int64_t car_id, CarPaintExceptions& out, std::string* error) {
  out = CarPaintExceptions{};
  if (car_id <= 0) return true;
  return db.each(
      "SELECT NoMirrors, NoWindows, NoHood, NoPaintableWing FROM CarExceptions WHERE CarID = ? LIMIT 1",
      {std::to_string(car_id)},
      [&](sqlite3_stmt* st) {
        out.found = true;
        out.no_mirrors = sqlite3_column_int(st, 0) != 0;
        out.no_windows = sqlite3_column_int(st, 1) != 0;
        out.no_hood = sqlite3_column_int(st, 2) != 0;
        out.no_paintable_wing = sqlite3_column_int(st, 3) != 0;
      },
      error);
}

bool resolve_visual_setup(const PartsDb& db, int64_t car_id, const FittedParts& fit, VisualSetup& out,
                          std::string* error, float tune_front_rh_norm, float tune_rear_rh_norm,
                          float tune_front_camber_deg, float tune_rear_camber_deg) {
  out = VisualSetup{};
  CarPhysics phys;
  if (!db.physics(car_id, fit, phys, error)) return false;

  out.active = true;
  out.tyre_media_override = per_car_tyre_media(phys.media_name);
  car_paint_exceptions(db, car_id, out.paint_exceptions, nullptr);

  // 1. Visual body & exhaust parts from List_PartsStrings (incl. Rally Lv4 bodykits).
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
    int* level_dst;
  };
  const PartStrSlot kSlots[] = {
      {"List_UpgradeCarBodyFrontBumper", "CarBodyID", "PartsStringId", 3, false, &out.front_bumper_level},
      {"List_UpgradeCarBodyRearBumper", "CarBodyID", "PartsStringId", 3, false, &out.rear_bumper_level},
      {"List_UpgradeCarBodySideSkirt", "CarBodyID", "PartsStringId", 3, false, nullptr},
      {"List_UpgradeCarBodyHood", "CarBodyID", "PartsStringId", 3, false, nullptr},
      {"List_UpgradeRearWing", "Ordinal", "PartsStringId", 0, false, &out.rear_wing_level},
      {"List_UpgradeCarBodyChassisStiffness", "CarbodyId", "PartsStringID", 3, false, nullptr},
      {"List_UpgradeCarBodyWeight", "CarBodyId", "PartsStringID", 3, false, nullptr},
      {"List_UpgradeEngineExhaust", "EngineID", "PartsStringID", 1, true, nullptr},
  };
  for (const auto& sl : kSlots) {
    int64_t ps_id = 0;
    int part_level = 0;
    auto fit_it = fit.find(sl.table);
    if (fit_it != fit.end() && fit_it->second >= 0) {
      db.each(std::string("SELECT CAST(") + sl.ps_col + " AS INTEGER), Level FROM " + sl.table + " WHERE Id = ? LIMIT 1",
              {std::to_string(fit_it->second)},
              [&](sqlite3_stmt* st) {
                ps_id = sqlite3_column_int64(st, 0);
                part_level = sqlite3_column_int(st, 1);
              },
              nullptr);
    } else {
      const int64_t key_val = sl.key_kind == 0 ? car_id : sl.key_kind == 1 ? engine_id : body_id;
      db.each(std::string("SELECT CAST(") + sl.ps_col + " AS INTEGER), Level FROM " + sl.table + " WHERE " +
                  sl.key_col + " = ? AND IsStock = 1 ORDER BY Id LIMIT 1",
              {std::to_string(key_val)},
              [&](sqlite3_stmt* st) {
                ps_id = sqlite3_column_int64(st, 0);
                part_level = sqlite3_column_int(st, 1);
              },
              nullptr);
    }
    if (sl.level_dst) *sl.level_dst = part_level;
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

  // 2. Stock & upgraded tyre widths, aspect ratios, and rim diameters.
  // Query un-overwritten stock Data_Car dimensions so rim/width upgrades can
  // preserve the stock rolling outer radius (thinning sidewall as rim grows).
  db.each(
      "SELECT FrontTireWidthMM, RearTireWidthMM, FrontWheelDiameterIN, RearWheelDiameterIN, "
      "FrontTireAspect, RearTireAspect FROM Data_Car WHERE Id = ? LIMIT 1",
      {std::to_string(car_id)},
      [&](sqlite3_stmt* st) {
        out.stock_tyre_width_mm[0] = sqlite3_column_int64(st, 0);
        out.stock_tyre_width_mm[1] = sqlite3_column_int64(st, 1);
        out.stock_rim_inches[0] = sqlite3_column_int64(st, 2);
        out.stock_rim_inches[1] = sqlite3_column_int64(st, 3);
        out.stock_tyre_aspect[0] = sqlite3_column_int64(st, 4);
        out.stock_tyre_aspect[1] = sqlite3_column_int64(st, 5);
      },
      nullptr);
  const int64_t fw = int64_t(std::llround(phys.get("Data_Car.FrontTireWidthMM", 0.0)));
  const int64_t rw = int64_t(std::llround(phys.get("Data_Car.RearTireWidthMM", 0.0)));
  const int64_t fr = int64_t(std::llround(phys.get("Data_Car.FrontWheelDiameterIN", 0.0)));
  const int64_t rr = int64_t(std::llround(phys.get("Data_Car.RearWheelDiameterIN", 0.0)));
  if (fw > 0) out.tyre_width_mm[0] = fw;
  if (rw > 0) out.tyre_width_mm[1] = rw;
  if (fr > 0) out.rim_inches[0] = fr;
  if (rr > 0) out.rim_inches[1] = rr;

  // 3. Aftermarket wheel (List_Wheels) + BrakeDrawOffset + WheelNormOffsets + WheelExceptions.
  int64_t wheel_id = int64_t(std::llround(phys.get("Data_Car.StockWheelID", 0.0)));
  if (auto it = fit.find("List_Wheels"); it != fit.end() && it->second > 0) wheel_id = it->second;
  if (wheel_id > 0) {
    WheelRow w;
    if (query_wheel_by_id(db, wheel_id, w, nullptr)) {
      out.wheel_id = w.id;
      out.wheel_media = lower(w.media_name);
      out.brake_draw_offset = float(w.brake_draw_offset);
      out.rim_offset_front = float(w.norm_offset);
      out.rim_offset_rear = float(w.norm_offset);
      out.wheel_is_chrome = w.is_chrome;
      out.wheel_is_stock = w.is_stock;
      out.wheel_is_exception = w.is_exception;
      out.wheel_is_paintable = w.is_paintable;
    }
  }
  out.tire_compound_level = std::clamp(int(std::llround(phys.get("List_UpgradeTireCompound.Level", 0.0))), 0, 5);
  out.tire_compound_id = int(std::llround(phys.get("List_UpgradeTireCompound.TireCompoundID", 0.0)));
  out.spring_damper_level = int(std::llround(phys.get("List_UpgradeSpringDamper.Level", 0.0)));
  out.has_rally_bodykit =
      (out.front_bumper_level == 4 || out.rear_bumper_level == 4 || out.rear_wing_level == 4);
  out.has_rally_suspension = (out.spring_damper_level == 4);
  out.has_rally_tires = (out.tire_compound_level == 5 || out.tire_compound_id == 9);

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
                                                      : phys.get("FrontSpringDamper.StaticCamber", 0.0);
  const double cam_r = tune_rear_camber_deg > -90.0f ? double(tune_rear_camber_deg)
                                                     : phys.get("RearSpringDamper.StaticCamber", 0.0);
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
