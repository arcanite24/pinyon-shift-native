#include "upgrade_shop.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

#include <sqlite3.h>

#include "drive/trainer.h"
#include "game_hud.h"

namespace carview {
namespace {

std::string resolve_localized_str(const std::string& raw) {
  if (raw.empty()) return {};
  if (raw.rfind("_&", 0) == 0) {
    const std::string resolved = game_text(raw);
    if (resolved.empty() || resolved == raw || resolved.rfind("_&", 0) == 0) return {};
    return resolved;
  }
  return raw;
}

std::string collapse_spaces(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  bool prev_sp = true;
  for (char c : sv) {
    const bool sp = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (sp) {
      if (!prev_sp) out += ' ';
      prev_sp = true;
    } else {
      out += c;
      prev_sp = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string col_text(sqlite3_stmt* st, int col) {
  const unsigned char* p = sqlite3_column_text(st, col);
  return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
}

std::string fallback_slot_name(int part_enum, const std::string& column) {
  switch (part_enum) {
    case 0: return "Engine Swap";
    case 1: return "Drivetrain Swap";
    case 2: return "Body Conversion";
    case 4: return "Brakes";
    case 5: return "Springs & Dampers";
    case 6: return "Front Anti-Roll Bar";
    case 7: return "Rear Anti-Roll Bar";
    case 8: return "Tire Compound";
    case 9: return "Rear Wing";
    case 10: return "Front Rim Size";
    case 11: return "Rear Rim Size";
    case 12: return "Camshaft";
    case 13: return "Valves";
    case 14: return "Displacement";
    case 15: return "Pistons & Compression";
    case 16: return "Fuel System";
    case 17: return "Ignition";
    case 18: return "Exhaust";
    case 19: return "Intake";
    case 20: return "Flywheel";
    case 21: return "Manifold";
    case 22: return "Restrictor Plate";
    case 23: return "Oil & Cooling";
    case 24: return "Single Turbo";
    case 25: return "Twin Turbo";
    case 26: return "Quad Turbo";
    case 27: return "Centrifugal Supercharger";
    case 28: return "Positive-Displacement Supercharger";
    case 29: return "Intercooler";
    case 30: return "Clutch";
    case 31: return "Transmission";
    case 32: return "Driveline";
    case 33: return "Differential";
    case 34: return "Front Bumper";
    case 35: return "Rear Bumper";
    case 36: return "Hood";
    case 37: return "Side Skirts";
    case 38: return "Front Tire Width";
    case 39: return "Rear Tire Width";
    case 40: return "Weight Reduction";
    case 41: return "Roll Cage";
    case 43: return "Aftermarket Rims";
    default: break;
  }
  if (column == "WheelStyle") return "Aftermarket Rims";
  std::string out;
  for (size_t i = 0; i < column.size(); ++i) {
    const char c = column[i];
    if (i > 0 && c >= 'A' && c <= 'Z' && !(column[i - 1] >= 'A' && column[i - 1] <= 'Z')) out += ' ';
    out += c;
  }
  return out;
}

std::string slot_name(const std::string& column) { return fallback_slot_name(-1, column); }

std::string credits_text(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return (v < 0 ? "-" : "") + s;
}

const char* tier_prefix(int part_enum, int level, bool is_stock) {
  if (is_stock || level <= 0) return "Stock";
  if (garage::is_rally_part(part_enum, level)) return "Horizon Rally";
  switch (level) {
    case 1: return "Street";
    case 2: return "Sport";
    case 3: return "Race";
    default: return "Pro";
  }
}

bool starts_with_icase(std::string_view s, std::string_view prefix) {
  if (prefix.empty() || s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  }
  return true;
}

std::string format_engine_swap_label(const std::string& raw_engine_name, const std::string& media_name,
                                     double disp_cc, const std::string& config) {
  const std::string cleaned = collapse_spaces(raw_engine_name);
  char disp_buf[24] = {};
  if (disp_cc >= 500.0) {
    std::snprintf(disp_buf, sizeof disp_buf, "%.1fL ", disp_cc / 1000.0);
  }
  if (!cleaned.empty()) {
    const bool has_liters = cleaned.find("L ") != std::string::npos || cleaned.find("l ") != std::string::npos;
    return has_liters ? cleaned : (std::string(disp_buf) + cleaned);
  }
  const std::string donor = media_name.empty() ? std::string() : car_name(media_name);
  if (!donor.empty() && donor != media_name) {
    return std::string(disp_buf) + (config.empty() ? "" : (config + " ")) + donor;
  }
  if (disp_buf[0] != '\0' || !config.empty()) {
    return collapse_spaces(std::string(disp_buf) + config + " Engine Swap");
  }
  return "Performance Engine Swap";
}

ShopCategory category_of(int part_enum) {
  if (part_enum <= 2) return ShopCategory::kConversions;
  if (part_enum == 24 || part_enum == 25 || part_enum == 27 || part_enum == 28) return ShopCategory::kConversions;
  if (part_enum >= 12 && part_enum <= 29) return ShopCategory::kEngine;
  if (part_enum == 4 || part_enum == 5 || part_enum == 6 || part_enum == 7 || part_enum == 40 || part_enum == 41)
    return ShopCategory::kPlatform;
  if (part_enum >= 30 && part_enum <= 33) return ShopCategory::kDrivetrain;
  if (part_enum == 8 || part_enum == 10 || part_enum == 11 || part_enum == 38 || part_enum == 39 || part_enum == 43)
    return ShopCategory::kTiresRims;
  return ShopCategory::kAero;  // 9 RearWing, 34..37 Front/RearBumper, Hood, SideSkirts
}

}  // namespace

const char* shop_category_name(ShopCategory cat) {
  switch (cat) {
    case ShopCategory::kEngine: return "ENGINE";
    case ShopCategory::kPlatform: return "PLATFORM & HANDLING";
    case ShopCategory::kDrivetrain: return "DRIVETRAIN";
    case ShopCategory::kTiresRims: return "TIRES & RIMS";
    case ShopCategory::kAero: return "AERO & APPEARANCE";
    case ShopCategory::kConversions: return "CONVERSIONS";
    case ShopCategory::kAutoUpgrade: return "AUTO UPGRADE";
    case ShopCategory::kTuning: return "TUNING";
    default: return "";
  }
}

bool UpgradeShop::open(const cars::PartsDb& db, garage::Garage& g, int64_t garage_id, std::string& why,
                       const cars::PhysicsSettings* phys_settings, const garage::PiConfig* pi_cfg,
                       const std::vector<garage::ClassBand>* bands, double price_multiplier) {
  open_ = false;
  changed_ = false;
  visual_dirty_ = false;
  db_ = &db;
  garage_ = &g;
  garage_id_ = garage_id;
  phys_settings_ = phys_settings;
  pi_cfg_ = pi_cfg;
  bands_ = bands;
  price_multiplier_ = std::clamp(price_multiplier, 0.25, 3.0);

  if (!reload_slots(why)) return false;

  // Build Auto Upgrade target classes (E=1 through R1=9).
  auto_targets_.clear();
  for (int cls = 1; cls <= 9; ++cls) auto_targets_.push_back(cls);
  const garage::Car* car = garage_->find(garage_id_);
  auto_index_ = 0;
  if (car) {
    for (size_t i = 0; i < auto_targets_.size(); ++i) {
      if (auto_targets_[i] > car->class_id) {
        auto_index_ = i;
        break;
      }
    }
  }

  category_ = ShopCategory::kEngine;
  if (category_slot_indices(category_).empty()) {
    for (int c = 0; c < int(ShopCategory::kAutoUpgrade); ++c) {
      if (!category_slot_indices(ShopCategory(c)).empty()) {
        category_ = ShopCategory(c);
        break;
      }
    }
  }
  selected_in_cat_ = 0;
  slider_index_ = 0;
  refresh_current_and_preview();
  open_ = true;
  return true;
}

bool UpgradeShop::reload_slots(std::string& why) {
  slots_.clear();
  sliders_.clear();
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  if (!car || !db_) {
    why = "no garage car " + std::to_string(garage_id_);
    return false;
  }

  std::vector<cars::PartRow> rows;
  if (!db_->options(car->car_id, garage::fitted(*car), rows, &why)) return false;

  drive::FestivalPacingConfig pacing;
  pacing.enabled = std::fabs(price_multiplier_ - 1.0) > 1e-4;
  pacing.price_multiplier = price_multiplier_;

  // Load authentic localized FH1 upgrade slot & part/tier/brand names from gamedb.slt + stringtables.
  std::map<int, std::string> db_slot_names;
  std::map<std::pair<int, int>, std::string> db_upgrade_names;
  std::map<int64_t, std::string> db_manuf_names;
  std::map<int64_t, std::string> db_engine_swap_names;
  std::map<int64_t, std::string> db_drive_swap_names;
  std::map<int64_t, int> db_front_tire_w, db_rear_tire_w, db_front_rim_in, db_rear_rim_in;

  db_->each(
      "SELECT PartEnum, Name FROM UpgradeTypes WHERE IsException = 0", {},
      [&](sqlite3_stmt* st) {
        const int pe = sqlite3_column_int(st, 0);
        const std::string nm = resolve_localized_str(col_text(st, 1));
        if (!nm.empty() && !db_slot_names.count(pe)) db_slot_names[pe] = nm;
      },
      nullptr);
  db_->each(
      "SELECT ut.PartEnum, u.Level, u.Name FROM Upgrades u "
      "JOIN UpgradeTypes ut ON CAST(u.TypeId AS INTEGER) = ut.id "
      "WHERE ut.IsException = 0 AND u.IsException = 0",
      {},
      [&](sqlite3_stmt* st) {
        const int pe = sqlite3_column_int(st, 0);
        const int lv = sqlite3_column_int(st, 1);
        const std::string nm = resolve_localized_str(col_text(st, 2));
        if (!nm.empty()) db_upgrade_names[{pe, lv}] = nm;
      },
      nullptr);
  db_->each(
      "SELECT Id, PartManufacturer FROM List_PartManufacturer", {},
      [&](sqlite3_stmt* st) {
        const int64_t mid = sqlite3_column_int64(st, 0);
        const std::string nm = resolve_localized_str(col_text(st, 1));
        if (mid > 0 && !nm.empty()) db_manuf_names[mid] = nm;
      },
      nullptr);
  const std::string car_id_str = std::to_string(car->car_id);
  db_->each(
      "SELECT ue.Id, de.EngineName, de.MediaName, "
      "COALESCE((SELECT d.Disp FROM List_UpgradeEngineDisplacement d "
      "          WHERE d.EngineID = ue.EngineID AND d.IsStock = 1 LIMIT 1), 0), "
      "COALESCE(ec.EngineConfig, '') "
      "FROM List_UpgradeEngine ue "
      "JOIN Data_Engine de ON de.EngineID = ue.EngineID "
      "LEFT JOIN List_EngineConfig ec ON ec.ConfigID = de.ConfigID "
      "WHERE ue.Ordinal = ?",
      {car_id_str},
      [&](sqlite3_stmt* st) {
        const int64_t id = sqlite3_column_int64(st, 0);
        db_engine_swap_names[id] =
            format_engine_swap_label(col_text(st, 1), col_text(st, 2), sqlite3_column_double(st, 3), col_text(st, 4));
      },
      nullptr);
  db_->each(
      "SELECT ud.Id, dt.DisplayName, dt.DriveType "
      "FROM List_UpgradeDrivetrain ud "
      "JOIN Data_Drivetrain dd ON dd.DrivetrainID = ud.DrivetrainID "
      "JOIN List_DriveType dt ON dt.ID = dd.DrivetypeID "
      "WHERE ud.Ordinal = ?",
      {car_id_str},
      [&](sqlite3_stmt* st) {
        const int64_t id = sqlite3_column_int64(st, 0);
        std::string dt = resolve_localized_str(col_text(st, 1));
        if (dt.empty()) dt = collapse_spaces(col_text(st, 2));
        if (!dt.empty()) db_drive_swap_names[id] = dt + " Drivetrain";
      },
      nullptr);
  db_->each(
      "SELECT Id, FrontWheelDiameter FROM List_UpgradeRimSizeFront WHERE Ordinal = ?", {car_id_str},
      [&](sqlite3_stmt* st) { db_front_rim_in[sqlite3_column_int64(st, 0)] = sqlite3_column_int(st, 1); }, nullptr);
  db_->each(
      "SELECT Id, RearWheelDiameter FROM List_UpgradeRimSizeRear WHERE Ordinal = ?", {car_id_str},
      [&](sqlite3_stmt* st) { db_rear_rim_in[sqlite3_column_int64(st, 0)] = sqlite3_column_int(st, 1); }, nullptr);

  int64_t body_id = 0;
  {
    cars::CarPhysics cphys;
    if (db_->physics(car->car_id, garage::fitted(*car), cphys, nullptr)) {
      body_id = int64_t(std::llround(cphys.get("List_UpgradeCarBody.CarBodyID", 0.0)));
    }
  }
  if (body_id > 0) {
    const std::string body_str = std::to_string(body_id);
    db_->each(
        "SELECT Id, FrontTireWidth FROM List_UpgradeCarBodyTireWidthFront WHERE CarBodyId = ?", {body_str},
        [&](sqlite3_stmt* st) { db_front_tire_w[sqlite3_column_int64(st, 0)] = sqlite3_column_int(st, 1); }, nullptr);
    db_->each(
        "SELECT Id, RearTireWidth FROM List_UpgradeCarBodyTireWidthRear WHERE CarBodyId = ?", {body_str},
        [&](sqlite3_stmt* st) { db_rear_tire_w[sqlite3_column_int64(st, 0)] = sqlite3_column_int(st, 1); }, nullptr);
  }

  for (const auto& s : cars::upgrade_slots()) {
    Slot slot;
    slot.part_enum = s.part_enum;
    slot.column = s.column;
    slot.table = s.table;
    slot.display_label = db_slot_names.count(s.part_enum) ? db_slot_names.at(s.part_enum)
                                                          : fallback_slot_name(s.part_enum, s.column);
    slot.category = category_of(s.part_enum);
    std::map<std::string, int> name_counts;
    for (auto r : rows) {
      if (r.part_enum == s.part_enum) {
        if (!r.is_stock && r.price > 0) r.price = pacing.scale_price(r.price, false);
        slot.parts.push_back(r);

        std::string pname;
        const int lookup_level = r.is_stock ? 0 : r.level;
        const auto uit = db_upgrade_names.find({s.part_enum, lookup_level});
        const std::string db_upg = (uit != db_upgrade_names.end()) ? uit->second : std::string();
        const auto mit = (r.manufacturer > 0) ? db_manuf_names.find(r.manufacturer) : db_manuf_names.end();
        const std::string brand = (mit != db_manuf_names.end()) ? mit->second : std::string();

        if (s.part_enum == 0) {
          if (r.is_stock) {
            pname = !db_upg.empty() ? db_upg : "Stock Engine";
          } else if (auto eit = db_engine_swap_names.find(r.id); eit != db_engine_swap_names.end()) {
            pname = eit->second;
          } else {
            pname = "Performance Engine Swap";
          }
        } else if (s.part_enum == 1) {
          if (r.is_stock) {
            pname = !db_upg.empty() ? db_upg : "Stock Drivetrain";
          } else if (auto dit = db_drive_swap_names.find(r.id); dit != db_drive_swap_names.end()) {
            pname = dit->second;
          } else {
            pname = "Drivetrain Conversion";
          }
        } else if (s.part_enum == 2) {
          pname = !db_upg.empty() ? db_upg : (r.is_stock ? "Stock Body" : "Widebody Conversion");
        } else if (s.part_enum == 38 || s.part_enum == 39) {
          const auto& wmap = (s.part_enum == 38) ? db_front_tire_w : db_rear_tire_w;
          const int mm = wmap.count(r.id) ? wmap.at(r.id) : 0;
          if (mm > 0) {
            pname = r.is_stock ? ("Stock (" + std::to_string(mm) + " mm)") : (std::to_string(mm) + " mm");
          } else if (!db_upg.empty()) {
            pname = db_upg;
          } else {
            pname = std::string(tier_prefix(s.part_enum, r.level, r.is_stock)) + " Width";
          }
        } else if (s.part_enum == 10 || s.part_enum == 11) {
          const auto& rmap = (s.part_enum == 10) ? db_front_rim_in : db_rear_rim_in;
          const int inch = rmap.count(r.id) ? rmap.at(r.id) : 0;
          if (inch > 0) {
            pname = r.is_stock ? ("Stock (" + std::to_string(inch) + " in)") : (std::to_string(inch) + " in");
          } else if (!db_upg.empty()) {
            pname = db_upg;
          } else {
            pname = std::string(tier_prefix(s.part_enum, r.level, r.is_stock)) + " Rim Size";
          }
        } else {
          const bool is_aero_body =
              (s.part_enum == 9 || s.part_enum == 34 || s.part_enum == 35 || s.part_enum == 36 || s.part_enum == 37);
          if (!db_upg.empty()) {
            pname = db_upg;
          } else if (r.is_stock) {
            pname = "Stock " + slot.display_label;
          } else if (garage::is_rally_part(s.part_enum, r.level)) {
            pname = "Horizon Rally " + slot.display_label;
          } else if (s.part_enum == 24 || s.part_enum == 25 || s.part_enum == 27 || s.part_enum == 28) {
            pname = (r.level <= 1) ? slot.display_label
                                   : (std::string(tier_prefix(s.part_enum, r.level, false)) + " " + slot.display_label);
          } else {
            pname = std::string(tier_prefix(s.part_enum, r.level, false)) + " " + slot.display_label;
          }
          if (!r.is_stock && !brand.empty() && is_aero_body && !starts_with_icase(pname, brand)) {
            pname = brand + " " + pname;
          }
        }
        const int dup = ++name_counts[pname];
        if (dup > 1) pname += " Style " + std::to_string(dup);
        slot.part_names.push_back(std::move(pname));
      }
    }
    if (slot.parts.empty()) continue;
    // Standard performance slots need at least 2 options; aspiration conversion
    // tables without a stock row are offered in Conversions when non-empty.
    const bool is_aspiration =
        (s.part_enum == 24 || s.part_enum == 25 || s.part_enum == 27 || s.part_enum == 28);
    if (!is_aspiration && slot.parts.size() < 2) continue;
    if (is_aspiration) {
      const bool has_stock = std::any_of(slot.parts.begin(), slot.parts.end(),
                                         [](const cars::PartRow& r) { return r.is_stock; });
      if (has_stock) slot.category = ShopCategory::kEngine;
    }
    slot.shown = fitted_index(slot);
    slots_.push_back(std::move(slot));
  }

  // Add List_Wheels (part_enum 43, WheelStyle) with stock wheel + all aftermarket rims.
  std::vector<cars::WheelRow> wheels;
  if (cars::query_aftermarket_wheels(*db_, wheels, nullptr, false) && !wheels.empty()) {
    cars::CarPhysics phys;
    int64_t stock_wid = 0;
    if (db_->physics(car->car_id, garage::fitted(*car), phys, nullptr)) {
      stock_wid = int64_t(std::llround(phys.get("Data_Car.StockWheelID", 0.0)));
    }
    Slot wslot;
    wslot.part_enum = 43;
    wslot.column = "WheelStyle";
    wslot.table = "List_Wheels";
    wslot.display_label = db_slot_names.count(43) ? db_slot_names.at(43) : fallback_slot_name(43, "WheelStyle");
    wslot.category = ShopCategory::kTiresRims;
    if (stock_wid > 0) {
      cars::PartRow sr;
      sr.part_enum = 43;
      sr.table = "List_Wheels";
      sr.id = stock_wid;
      sr.level = 0;
      sr.is_stock = true;
      sr.price = 0;
      wslot.parts.push_back(sr);
      wslot.part_names.push_back("Stock Rims");
    }
    for (const auto& w : wheels) {
      cars::PartRow pr;
      pr.part_enum = 43;
      pr.table = "List_Wheels";
      pr.id = w.id;
      pr.level = 1;
      pr.is_stock = false;
      pr.price = pacing.scale_price(w.price, false);
      pr.manufacturer = w.manufacturer_id;
      wslot.parts.push_back(pr);
      wslot.part_names.push_back(w.display_name.empty() ? w.media_name : w.display_name);
    }
    wslot.shown = fitted_index(wslot);
    slots_.push_back(std::move(wslot));
  }

  // Build 35-slider Car Tuning definitions from fitted physics.
  cars::CarPhysics phys;
  if (db_->physics(car->car_id, garage::fitted(*car), phys, nullptr)) {
    sliders_ = garage::build_tune_sliders(phys, car->tune);
  }

  if (slots_.empty()) {
    why = "no upgrades for this car";
    return false;
  }
  return true;
}

size_t UpgradeShop::fitted_index(const Slot& s) const {
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  if (car) {
    const auto it = car->slots.find(s.column);
    if (it != car->slots.end() && it->second > 0) {
      for (size_t k = 0; k < s.parts.size(); ++k)
        if (s.parts[k].id == it->second) return k;
    }
  }
  for (size_t k = 0; k < s.parts.size(); ++k)
    if (s.parts[k].is_stock) return k;
  return 0;
}

std::vector<size_t> UpgradeShop::category_slot_indices(ShopCategory cat) const {
  std::vector<size_t> idx;
  for (size_t i = 0; i < slots_.size(); ++i)
    if (slots_[i].category == cat) idx.push_back(i);
  return idx;
}

void UpgradeShop::refresh_current_and_preview() {
  has_preview_pi_ = false;
  auto_plan_ = garage::AutoUpgradePlan{};
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  if (!car || !db_ || !phys_settings_ || !pi_cfg_ || !bands_) return;

  garage::compute_pi_for_car(*db_, car->car_id, garage::fitted(*car), *phys_settings_, *pi_cfg_, *bands_, current_pi_,
                             nullptr);
  preview_pi_ = current_pi_;
  has_preview_pi_ = true;

  if (category_ == ShopCategory::kAutoUpgrade) {
    if (auto_index_ < auto_targets_.size()) {
      auto_plan_ = garage::plan_auto_upgrade(*garage_, *db_, garage_id_, auto_targets_[auto_index_], 0,
                                             *phys_settings_, *pi_cfg_, *bands_);
      if (auto_plan_.ok) preview_pi_ = auto_plan_.result;
    }
    return;
  }
  if (category_ == ShopCategory::kTuning) return;

  const auto idxs = category_slot_indices(category_);
  if (idxs.empty() || selected_in_cat_ >= idxs.size()) return;
  const Slot& s = slots_[idxs[selected_in_cat_]];
  if (s.shown >= s.parts.size() || s.shown == fitted_index(s)) return;

  // Simulate fitting the previewed part on a temporary Garage copy so
  // engine/drivetrain/aspiration conversions re-key accurately!
  garage::Garage tmp = *garage_;
  int64_t dummy_cr = 1000000000;
  garage::PiResult tmp_pi;
  const garage::FitResult fr =
      garage::fit_part_full(tmp, *db_, garage_id_, s.parts[s.shown], s.column.c_str(), 0, dummy_cr, phys_settings_,
                            pi_cfg_, bands_, &tmp_pi);
  if (fr.ok) preview_pi_ = tmp_pi;
}

void UpgradeShop::move_category(int by) {
  const int n = int(ShopCategory::kCount);
  category_ = ShopCategory(((int(category_) + by) % n + n) % n);
  selected_in_cat_ = 0;
  refresh_current_and_preview();
}

void UpgradeShop::set_category(ShopCategory cat) {
  category_ = cat;
  selected_in_cat_ = 0;
  refresh_current_and_preview();
}

void UpgradeShop::move_slot(int by) {
  if (category_ == ShopCategory::kAutoUpgrade) {
    if (auto_targets_.empty()) return;
    const int n = int(auto_targets_.size());
    auto_index_ = size_t(((int(auto_index_) + by) % n + n) % n);
    refresh_current_and_preview();
    return;
  }
  if (category_ == ShopCategory::kTuning) {
    if (sliders_.empty()) return;
    const int n = int(sliders_.size());
    slider_index_ = size_t(((int(slider_index_) + by) % n + n) % n);
    return;
  }
  const auto idxs = category_slot_indices(category_);
  if (idxs.empty()) return;
  const int n = int(idxs.size());
  selected_in_cat_ = size_t(((int(selected_in_cat_) + by) % n + n) % n);
  refresh_current_and_preview();
}

void UpgradeShop::move_part(int by) {
  if (category_ == ShopCategory::kAutoUpgrade) {
    move_slot(by);
    return;
  }
  if (category_ == ShopCategory::kTuning) {
    garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
    if (!car || !db_ || slider_index_ >= sliders_.size()) return;
    garage::TuneSlider& sl = sliders_[slider_index_];
    if (!sl.unlocked) return;
    cars::CarPhysics phys;
    if (!db_->physics(car->car_id, garage::fitted(*car), phys, nullptr)) return;
    const double next_val = std::clamp(sl.value + double(by) * sl.step, sl.min_val, sl.max_val);
    if (garage::set_tune_slider(car->tune, phys, sl.key, next_val)) {
      sl.value = next_val;
      sl.tuned = true;
      changed_ = true;
      visual_dirty_ = (sl.category == garage::TuneCategory::kSprings || sl.category == garage::TuneCategory::kAlignment);
    }
    return;
  }
  const auto idxs = category_slot_indices(category_);
  if (idxs.empty() || selected_in_cat_ >= idxs.size()) return;
  Slot& s = slots_[idxs[selected_in_cat_]];
  if (s.parts.empty()) return;
  const int n = int(s.parts.size());
  s.shown = size_t(((int(s.shown) + by) % n + n) % n);
  visual_dirty_ = true;
  refresh_current_and_preview();
}

bool UpgradeShop::fit(int discount_percent, int64_t& credits, std::string& message) {
  if (!garage_ || !db_) return false;

  if (category_ == ShopCategory::kAutoUpgrade) {
    if (!phys_settings_ || !pi_cfg_ || !bands_ || auto_index_ >= auto_targets_.size()) {
      message = "auto upgrade unavailable";
      return false;
    }
    const auto plan = garage::plan_auto_upgrade(*garage_, *db_, garage_id_, auto_targets_[auto_index_],
                                                discount_percent, *phys_settings_, *pi_cfg_, *bands_);
    if (!garage::apply_auto_upgrade(*garage_, *db_, garage_id_, plan, discount_percent, credits, *phys_settings_,
                                    *pi_cfg_, *bands_, &message)) {
      return false;
    }
    changed_ = true;
    visual_dirty_ = true;
    std::string reload_err;
    reload_slots(reload_err);
    refresh_current_and_preview();
    return true;
  }

  if (category_ == ShopCategory::kTuning) {
    reset_tuning();
    message = "tuning reset to stock";
    return true;
  }

  const auto idxs = category_slot_indices(category_);
  if (idxs.empty() || selected_in_cat_ >= idxs.size()) return false;
  Slot& s = slots_[idxs[selected_in_cat_]];
  if (s.shown >= s.parts.size()) return false;
  const cars::PartRow& part = s.parts[s.shown];
  if (s.shown == fitted_index(s)) {
    message = "already fitted";
    return false;
  }

  garage::PiResult pi_res;
  const garage::FitResult r =
      garage::fit_part_full(*garage_, *db_, garage_id_, part, s.column.c_str(), discount_percent, credits,
                            phys_settings_, pi_cfg_, bands_, (phys_settings_ && pi_cfg_ && bands_) ? &pi_res : nullptr);
  if (!r.ok) {
    message = "not enough CR (" + credits_text(garage::discounted_price(part.price, discount_percent)) + " CR)";
    return false;
  }

  changed_ = true;
  visual_dirty_ = true;
  const std::string sname = s.display_label.empty() ? slot_name(s.column) : s.display_label;
  const std::string pname = s.shown < s.part_names.size() ? s.part_names[s.shown] : sname;
  message = sname + " [" + pname + "]" +
            (r.refit ? " refitted" : " bought for " + credits_text(r.charged) + " CR");

  if (r.rekeyed) {
    std::string reload_err;
    reload_slots(reload_err);
    const auto new_idxs = category_slot_indices(category_);
    if (!new_idxs.empty()) selected_in_cat_ = std::min(selected_in_cat_, new_idxs.size() - 1);
  } else {
    // Refresh tuning unlock masks (e.g., fitting Race Springs/Diff/Trans unlocks tuning sliders).
    if (const garage::Car* car = garage_->find(garage_id_)) {
      cars::CarPhysics phys;
      if (db_->physics(car->car_id, garage::fitted(*car), phys, nullptr)) {
        sliders_ = garage::build_tune_sliders(phys, car->tune);
      }
    }
  }
  refresh_current_and_preview();
  return true;
}

bool UpgradeShop::sell_shown_part(int64_t& credits, std::string& message) {
  if (!garage_ || category_ == ShopCategory::kAutoUpgrade || category_ == ShopCategory::kTuning) {
    message = "select a part to sell";
    return false;
  }
  const auto idxs = category_slot_indices(category_);
  if (idxs.empty() || selected_in_cat_ >= idxs.size()) return false;
  const Slot& s = slots_[idxs[selected_in_cat_]];
  if (s.shown >= s.parts.size()) return false;
  const cars::PartRow& part = s.parts[s.shown];
  int64_t stock_id = -1;
  for (const auto& sp : s.parts)
    if (sp.is_stock) { stock_id = sp.id; break; }
  const garage::SellPartResult sr =
      garage::sell_part(*garage_, garage_id_, part.part_enum, part.id, stock_id, s.column.c_str(), credits);
  if (!sr.ok) {
    message = sr.reason;
    return false;
  }
  changed_ = true;
  visual_dirty_ = true;
  if (phys_settings_ && pi_cfg_ && bands_) {
    if (garage::Car* car = garage_->find(garage_id_))
      garage::refresh_car_pi(*car, *db_, *phys_settings_, *pi_cfg_, *bands_, nullptr, nullptr);
  }
  refresh_current_and_preview();
  const std::string sname = s.display_label.empty() ? slot_name(s.column) : s.display_label;
  message = "sold " + sname + " part for +" + credits_text(sr.payout) + " CR";
  return true;
}

void UpgradeShop::reset_tuning() {
  garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  if (!car || !db_) return;
  garage::reset_tune(car->tune);
  cars::CarPhysics phys;
  if (db_->physics(car->car_id, garage::fitted(*car), phys, nullptr)) {
    sliders_ = garage::build_tune_sliders(phys, car->tune);
  }
  changed_ = true;
  visual_dirty_ = true;
}

garage::RallyKitPlan UpgradeShop::rally_kit_plan(int discount_percent, bool include_optional_aero) const {
  if (!open_ || !garage_ || !db_) return {};
  return garage::plan_rally_kit(*garage_, *db_, garage_id_, discount_percent, include_optional_aero, phys_settings_,
                                pi_cfg_, bands_);
}

bool UpgradeShop::fit_rally_kit(int discount_percent, int64_t& credits, std::string& message,
                                bool include_optional_aero) {
  if (!open_ || !garage_ || !db_) return false;
  const auto plan = rally_kit_plan(discount_percent, include_optional_aero);
  if (!garage::apply_rally_kit(*garage_, *db_, garage_id_, plan, discount_percent, credits, phys_settings_, pi_cfg_,
                               bands_, &message)) {
    return false;
  }
  changed_ = true;
  visual_dirty_ = true;
  std::string ignored;
  reload_slots(ignored);
  refresh_current_and_preview();
  return true;
}

int UpgradeShop::active_part_enum() const {
  if (category_ == ShopCategory::kAutoUpgrade || category_ == ShopCategory::kTuning) return -1;
  const auto idxs = category_slot_indices(category_);
  if (idxs.empty() || selected_in_cat_ >= idxs.size()) return -1;
  return slots_[idxs[selected_in_cat_]].part_enum;
}

size_t UpgradeShop::selected_row() const {
  if (category_ == ShopCategory::kAutoUpgrade) return auto_index_;
  if (category_ == ShopCategory::kTuning) return slider_index_;
  return selected_in_cat_;
}

std::vector<UpgradeShop::SlotView> UpgradeShop::visible_rows(int discount_percent) const {
  std::vector<SlotView> out;
  if (category_ == ShopCategory::kAutoUpgrade) {
    for (size_t i = 0; i < auto_targets_.size(); ++i) {
      const int cls = auto_targets_[i];
      SlotView v;
      const std::string cname =
          (bands_ && cls < int(bands_->size())) ? (*bands_)[size_t(cls)].name : ("Class " + std::to_string(cls));
      const int ceil_pi =
          (bands_ && cls < int(bands_->size())) ? int(std::lround((*bands_)[size_t(cls)].shown_max)) : (cls * 100);
      v.label = "Auto Upgrade to " + cname + " " + std::to_string(ceil_pi);
      if (i == auto_index_ && auto_plan_.ok) {
        v.part_label = std::to_string(auto_plan_.steps.size()) + " parts -> " + cname + " " +
                       std::to_string(auto_plan_.result.shown_pi);
        v.state = credits_text(auto_plan_.total_cost) + " CR";
        v.price = auto_plan_.total_cost;
      } else {
        v.part_label = "Target " + cname + " Class";
        v.state = "SELECT";
      }
      out.push_back(std::move(v));
    }
    return out;
  }
  if (category_ == ShopCategory::kTuning) {
    for (const auto& sl : sliders_) {
      SlotView v;
      v.label = std::string(garage::tune_category_name(sl.category)) + " : " + sl.label;
      char valbuf[64];
      std::snprintf(valbuf, sizeof valbuf, "%.2f %s", sl.value, sl.unit.c_str());
      v.part_label = valbuf;
      v.state = sl.unlocked ? "TUNABLE" : "LOCKED (NEED RACE PART)";
      v.fitted = sl.unlocked;
      out.push_back(std::move(v));
    }
    return out;
  }
  const auto idxs = category_slot_indices(category_);
  for (size_t idx : idxs) {
    const Slot& s = slots_[idx];
    const cars::PartRow& p = s.parts[std::min(s.shown, s.parts.size() - 1)];
    const bool is_fit = s.shown == fitted_index(s);
    const bool is_own = garage_ && garage_->owns_part(garage_id_, p.part_enum, p.id);
    const int64_t disc_p = garage::discounted_price(p.price, discount_percent);
    SlotView v;
    v.label = s.display_label.empty() ? slot_name(s.column) : s.display_label;
    v.part_label = s.shown < s.part_names.size()
                       ? s.part_names[s.shown]
                       : (std::string(tier_prefix(s.part_enum, p.level, p.is_stock)) + " " + v.label);
    v.level = p.level;
    v.price = disc_p;
    v.fitted = is_fit;
    v.owned = is_own;
    v.is_stock = p.is_stock;
    v.part_enum = s.part_enum;
    v.state = is_fit ? "FITTED" : p.is_stock ? "STOCK" : is_own ? "OWNED" : (credits_text(disc_p) + " CR");
    out.push_back(std::move(v));
  }
  return out;
}

bool UpgradeShop::preview_visual(cars::VisualSetup& out) const {
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  if (!car || !db_) return false;
  cars::FittedParts fit = garage::fitted(*car);
  if (category_ != ShopCategory::kAutoUpgrade && category_ != ShopCategory::kTuning) {
    const auto idxs = category_slot_indices(category_);
    if (!idxs.empty() && selected_in_cat_ < idxs.size()) {
      const Slot& s = slots_[idxs[selected_in_cat_]];
      if (s.shown < s.parts.size()) fit[s.table] = s.parts[s.shown].id;
    }
  }
  return cars::resolve_visual_setup(*db_, car->car_id, fit, out, nullptr, float(car->tune.front_ride_height),
                                    float(car->tune.rear_ride_height), float(car->tune.front_camber_deg),
                                    float(car->tune.rear_camber_deg));
}

void UpgradeShop::fill(const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                       std::string& title, std::vector<std::string>& lines, int& selected) const {
  lines.clear();
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  char head[192];
  const double pi = has_preview_pi_ ? preview_pi_.raw_pi : (car ? car->performance_index : 0.0);
  std::snprintf(head, sizeof head, "upgrades [%s]  %s %d  %s CR  discount %d%%", shop_category_name(category_),
                bands.empty() ? "?" : bands[size_t(garage::class_of(pi, bands))].name.c_str(),
                garage::shown_index(pi, bands), credits_text(credits).c_str(), discount_percent);
  title = head;

  const auto rows = visible_rows(discount_percent);
  const int n = int(rows.size());
  const int sel = n > 0 ? int(std::min(selected_row(), rows.size() - 1)) : 0;
  const int first = std::clamp(sel - 3, 0, std::max(0, n - 7));
  for (int k = first; k < std::min(n, first + 7); ++k) {
    const auto& r = rows[size_t(k)];
    char line[192];
    std::snprintf(line, sizeof line, "%s  [%s]  %s", r.label.c_str(), r.part_label.c_str(), r.state.c_str());
    lines.push_back(line);
  }
  selected = sel - first;
}

}  // namespace carview
