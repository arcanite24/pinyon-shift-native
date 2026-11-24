#include "garage/garage.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "cars/mods.h"
#include "cars/sources.h"
#include "garage/tuning.h"

namespace garage {
namespace {

bool is_aspiration_enum(int e) { return e == 24 || e == 25 || e == 27 || e == 28; }

const char* column_for_enum(int part_enum) {
  for (const auto& s : cars::upgrade_slots())
    if (s.part_enum == part_enum) return s.column;
  if (part_enum == 43) return "WheelStyle";
  return nullptr;
}

int slot_key_for_enum(int part_enum) {
  for (const auto& s : cars::upgrade_slots())
    if (s.part_enum == part_enum) return s.key;
  return 0;
}

}  // namespace

Car* Garage::find(int64_t id) {
  for (auto& c : cars)
    if (c.id == id) return &c;
  return nullptr;
}

const Car* Garage::find(int64_t id) const {
  for (const auto& c : cars)
    if (c.id == id) return &c;
  return nullptr;
}

const Car* Garage::selected_car() const {
  if (const Car* c = find(selected)) return c;
  const Car* lowest = nullptr;
  for (const auto& c : cars)
    if (!lowest || c.id < lowest->id) lowest = &c;
  return lowest;
}

bool Garage::owns_part(int64_t garage_id, int part_enum, int64_t part_id) const {
  return find_bought(garage_id, part_enum, part_id) != nullptr;
}

const Bought* Garage::find_bought(int64_t garage_id, int part_enum, int64_t part_id) const {
  for (const auto& b : bought)
    if (b.garage_id == garage_id && b.part_enum == part_enum && b.part_id == part_id) return &b;
  return nullptr;
}

bool Garage::toggle_favorite(int64_t garage_id) {
  Car* c = find(garage_id);
  if (!c) return false;
  c->favorite = !c->favorite;
  return c->favorite;
}

bool Garage::is_club_shared(int64_t garage_id) const {
  if (garage_id <= 0) return false;
  return std::find(club_shared_garage_ids.begin(), club_shared_garage_ids.end(), garage_id) !=
         club_shared_garage_ids.end();
}

bool Garage::toggle_club_shared(int64_t garage_id) {
  if (garage_id <= 0 || !find(garage_id)) return false;
  auto it = std::find(club_shared_garage_ids.begin(), club_shared_garage_ids.end(), garage_id);
  if (it != club_shared_garage_ids.end()) {
    club_shared_garage_ids.erase(it);
    if (club_borrowed_garage_id == garage_id) {
      club_borrowed_garage_id = 0;
      club_return_garage_id = 0;
    }
    return false;
  }
  club_shared_garage_ids.push_back(garage_id);
  return true;
}

bool Garage::borrow_club_car(int64_t garage_id) {
  if (!is_club_shared(garage_id) || !find(garage_id)) return false;
  if (club_borrowed_garage_id <= 0 && selected > 0 && selected != garage_id) {
    club_return_garage_id = selected;
  }
  club_borrowed_garage_id = garage_id;
  selected = garage_id;
  return true;
}

bool Garage::return_borrowed_club_car() {
  if (club_borrowed_garage_id <= 0) return false;
  const int64_t ret = club_return_garage_id;
  club_borrowed_garage_id = 0;
  club_return_garage_id = 0;
  if (ret > 0 && find(ret)) {
    selected = ret;
  }
  return true;
}

cars::FittedParts fitted(const Car& car) {
  cars::FittedParts f;
  for (const auto& s : cars::upgrade_slots()) {
    const auto it = car.slots.find(s.column);
    if (it == car.slots.end()) continue;
    if (it->second >= 0) {
      f[s.table] = it->second;
    } else if (is_aspiration_enum(s.part_enum) || s.part_enum == 29) {
      // Preserve explicit -1 for induction tables so aspiration swaps override stock induction.
      f[s.table] = -1;
    }
  }
  if (auto it = car.slots.find("WheelStyle"); it != car.slots.end() && it->second > 0) {
    f["List_Wheels"] = it->second;
  }
  return f;
}

int64_t discounted_price(int64_t price, int discount_percent) {
  if (discount_percent <= 0) return price;
  const float d = float(std::min(discount_percent, 100)) / 100.0f;
  return int64_t(std::floor((1.0f - d) * float(price) + 0.5f));
}

int upgrade_discount_percent(int discount_signs_found, int /*affinity_discount_percent*/) {
  return std::clamp(discount_signs_found, 0, 100);
}

int64_t upgrade_part_price(int64_t price, int discount_signs_found, int affinity_discount_percent) {
  return discounted_price(price, upgrade_discount_percent(discount_signs_found, affinity_discount_percent));
}

FitResult fit_part(Garage& g, int64_t garage_id, const cars::PartRow& part, const char* column, int discount_percent,
                   int64_t& credits) {
  FitResult r;
  Car* car = g.find(garage_id);
  if (!car) return r;
  // A stock row, explicit removal (id < 0), or previously bought part is refitted for free.
  if (part.is_stock || part.id < 0 || g.owns_part(garage_id, part.part_enum, part.id)) {
    r.refit = true;
  } else {
    r.charged = discounted_price(part.price, discount_percent);
    if (credits < r.charged) return r;
    credits -= r.charged;
    g.bought.push_back({garage_id, part.part_enum, part.id, part.manufacturer, r.charged});
    car->parts_value += r.charged;
  }
  car->slots[column] = part.id;
  r.ok = true;
  return r;
}

FitResult fit_part_full(Garage& g, const cars::PartsDb& db, int64_t garage_id, const cars::PartRow& part,
                        const char* column, int discount_percent, int64_t& credits,
                        const cars::PhysicsSettings* phys_settings, const PiConfig* pi_cfg,
                        const std::vector<ClassBand>* bands, PiResult* pi_out) {
  FitResult r;
  Car* car = g.find(garage_id);
  if (!car) return r;

  const int64_t prev_slot = car->slots.count(column) ? car->slots[column] : -1;
  r = fit_part(g, garage_id, part, column, discount_percent, credits);
  if (!r.ok) return r;

  // Handle conversions that re-key dependent sub-part slots (SPEC-GARAGE-UPGRADES):
  //   0 = Engine swap -> re-keys engine sub-parts (key == 1, enums 12..29)
  //   1 = Drivetrain swap -> re-keys drivetrain sub-parts (key == 2, enums 30..33)
  //   2 = CarBody swap -> re-keys body sub-parts (key == 3, enums 34..41)
  //   24, 25, 27, 28 = Aspiration conversion -> clears conflicting induction slots to -1
  if (part.part_enum <= 2 && part.id != prev_slot && db.is_open()) {
    const int target_key = part.part_enum == 0 ? 1 : part.part_enum == 1 ? 2 : 3;
    // Clear existing sub-part slots of that family before querying new stock rows.
    for (const auto& s : cars::upgrade_slots()) {
      if (s.key == target_key) car->slots.erase(s.column);
    }
    std::vector<cars::PartRow> new_opts;
    if (db.options(car->car_id, fitted(*car), new_opts, nullptr)) {
      for (const auto& s : cars::upgrade_slots()) {
        if (s.key != target_key) continue;
        int64_t best_stock = -1;
        for (const auto& o : new_opts) {
          if (o.part_enum == s.part_enum && o.is_stock && (best_stock < 0 || o.id < best_stock)) {
            best_stock = o.id;
          }
        }
        if (best_stock >= 0) {
          car->slots[s.column] = best_stock;
        } else if (is_aspiration_enum(s.part_enum) || s.part_enum == 29) {
          car->slots[s.column] = -1;
        }
      }
      r.rekeyed = true;
    }
  } else if (is_aspiration_enum(part.part_enum)) {
    // Fitting one forced-induction type removes any other forced-induction type.
    for (int e : {24, 25, 27, 28}) {
      if (e == part.part_enum) continue;
      if (const char* c = column_for_enum(e)) car->slots[c] = -1;
    }
    // If removing induction (part.id < 0), also clear intercooler.
    if (part.id < 0) car->slots["Intercooler"] = -1;
    r.rekeyed = true;
  }

  if (phys_settings && pi_cfg && bands && db.is_open()) {
    refresh_car_pi(*car, db, *phys_settings, *pi_cfg, *bands, pi_out);
  }
  return r;
}

bool revert_to_naturally_aspirated(Garage& g, const cars::PartsDb* db, int64_t garage_id,
                                   const cars::PhysicsSettings* phys_settings, const PiConfig* pi_cfg,
                                   const std::vector<ClassBand>* bands, PiResult* pi_out) {
  Car* car = g.find(garage_id);
  if (!car) return false;
  bool removed_any = false;
  for (int e : {24, 25, 27, 28, 29}) {
    if (const char* col = column_for_enum(e)) {
      auto it = car->slots.find(col);
      if (it != car->slots.end() && it->second > 0) {
        it->second = -1;
        removed_any = true;
      }
    }
  }
  if (!removed_any) return false;
  if (db && db->is_open() && phys_settings && pi_cfg && bands) {
    refresh_car_pi(*car, *db, *phys_settings, *pi_cfg, *bands, pi_out);
  }
  return true;
}

bool refresh_car_pi(Car& car, const cars::PartsDb& db, const cars::PhysicsSettings& phys_settings,
                    const PiConfig& pi_cfg, const std::vector<ClassBand>& bands, PiResult* out, std::string* error) {
  PiResult res;
  if (!compute_pi_for_car(db, car.car_id, fitted(car), phys_settings, pi_cfg, bands, res, error)) return false;
  car.performance_index = res.raw_pi;
  car.class_id = res.class_id;
  car.cached_stats = res.stats;
  if (out) *out = res;
  return true;
}

bool buy_car(Garage& g, int64_t car_id, int64_t price, double performance_index, int class_id, int colour,
             const std::map<std::string, int64_t>& stock, int64_t& credits, int64_t* new_id) {
  if (credits < price) return false;
  credits -= price;
  int64_t last = g.last_id;
  for (const auto& c : g.cars) last = std::max(last, c.id);
  Car c;
  c.id = last + 1, c.car_id = car_id, c.performance_index = performance_index, c.class_id = class_id;
  c.colour = colour, c.slots = stock;
  c.is_prize = false;
  g.cars.push_back(std::move(c));
  g.last_id = last + 1;
  g.selected = last + 1;
  if (new_id) *new_id = last + 1;
  return true;
}

bool award_prize_car(Garage& g, int64_t car_id, double performance_index, int class_id, int colour,
                     const std::map<std::string, int64_t>& stock, int64_t* new_id, bool select_it) {
  int64_t last = g.last_id;
  for (const auto& c : g.cars) last = std::max(last, c.id);
  Car c;
  c.id = last + 1, c.car_id = car_id, c.performance_index = performance_index, c.class_id = class_id;
  c.colour = colour, c.slots = stock;
  c.is_prize = true;
  g.cars.push_back(std::move(c));
  g.last_id = last + 1;
  if (select_it || g.selected <= 0) g.selected = last + 1;
  if (new_id) *new_id = last + 1;
  return true;
}

int64_t sell_car_value(int64_t base_cost, int64_t parts_value, bool is_prize) {
  if (is_prize) return kPrizeCarSellValue;
  // SPEC-GARAGE-UPGRADES: floor(SellValueScale * (BaseCost + PartsValue)), SellValueScale = 0.5.
  return int64_t(std::floor(0.5 * double(std::max<int64_t>(0, base_cost) + std::max<int64_t>(0, parts_value))));
}

SellCarResult sell_car(Garage& g, int64_t garage_id, int64_t base_cost, int64_t& credits) {
  SellCarResult res;
  if (g.cars.size() <= 1) {
    res.reason = "cannot sell your last car";
    return res;
  }
  if (g.selected == garage_id) {
    res.reason = "cannot sell your current car";
    return res;
  }
  auto it = std::find_if(g.cars.begin(), g.cars.end(), [&](const Car& c) { return c.id == garage_id; });
  if (it == g.cars.end()) {
    res.reason = "car not found in garage";
    return res;
  }
  res.payout = sell_car_value(base_cost, it->parts_value, it->is_prize);
  credits += res.payout;
  g.cars.erase(it);
  g.bought.erase(
      std::remove_if(g.bought.begin(), g.bought.end(), [&](const Bought& b) { return b.garage_id == garage_id; }),
      g.bought.end());
  g.club_shared_garage_ids.erase(
      std::remove(g.club_shared_garage_ids.begin(), g.club_shared_garage_ids.end(), garage_id),
      g.club_shared_garage_ids.end());
  if (g.club_borrowed_garage_id == garage_id) g.club_borrowed_garage_id = 0;
  if (g.club_return_garage_id == garage_id) g.club_return_garage_id = 0;
  res.ok = true;
  return res;
}

int64_t sell_part_value(int64_t price_paid) {
  // GameTunableSettings.ini: CarValueScales\RestockingValueScale = 0.5.
  return int64_t(std::floor(0.5 * double(std::max<int64_t>(0, price_paid))));
}

SellPartResult sell_part(Garage& g, int64_t garage_id, int part_enum, int64_t part_id, int64_t stock_part_id,
                         const char* column, int64_t& credits, const cars::PartsDb* db) {
  SellPartResult res;
  Car* car = g.find(garage_id);
  if (!car) {
    res.reason = "car not found in garage";
    return res;
  }
  if (part_id == stock_part_id) {
    res.reason = "cannot sell stock part";
    return res;
  }
  auto it = std::find_if(g.bought.begin(), g.bought.end(), [&](const Bought& b) {
    return b.garage_id == garage_id && b.part_enum == part_enum && b.part_id == part_id;
  });
  if (it == g.bought.end()) {
    res.reason = "part is not owned";
    return res;
  }
  res.payout = sell_part_value(it->price_paid);
  credits += res.payout;
  car->parts_value = std::max<int64_t>(0, car->parts_value - it->price_paid);
  g.bought.erase(it);

  bool was_fitted = false;
  if (column) {
    const auto sit = car->slots.find(column);
    if (sit != car->slots.end() && sit->second == part_id) {
      was_fitted = true;
      if (stock_part_id >= 0)
        car->slots[column] = stock_part_id;
      else
        car->slots[column] = -1;
    }
  }

  // Clean up orphaned dependent sub-parts when selling an Engine/Drivetrain/Body
  // conversion or a forced-induction aspiration conversion that reverts to NA.
  if (was_fitted && part_enum <= 2) {
    const int target_key = part_enum == 0 ? 1 : part_enum == 1 ? 2 : 3;
    for (const auto& s : cars::upgrade_slots()) {
      if (s.key != target_key) continue;
      if (is_aspiration_enum(s.part_enum) || s.part_enum == 29)
        car->slots[s.column] = -1;
      else
        car->slots.erase(s.column);
    }
    std::set<std::pair<int, int64_t>> valid_opts;
    if (db && db->is_open()) {
      std::vector<cars::PartRow> new_opts;
      if (db->options(car->car_id, fitted(*car), new_opts, nullptr)) {
        for (const auto& o : new_opts) valid_opts.insert({o.part_enum, o.id});
        for (const auto& s : cars::upgrade_slots()) {
          if (s.key != target_key) continue;
          int64_t best_stock = -1;
          for (const auto& o : new_opts) {
            if (o.part_enum == s.part_enum && o.is_stock && (best_stock < 0 || o.id < best_stock)) {
              best_stock = o.id;
            }
          }
          if (best_stock >= 0)
            car->slots[s.column] = best_stock;
          else if (is_aspiration_enum(s.part_enum) || s.part_enum == 29)
            car->slots[s.column] = -1;
        }
      }
    }
    for (auto bit = g.bought.begin(); bit != g.bought.end();) {
      if (bit->garage_id == garage_id && slot_key_for_enum(bit->part_enum) == target_key &&
          (valid_opts.empty() || !valid_opts.count({bit->part_enum, bit->part_id}))) {
        const int64_t opay = sell_part_value(bit->price_paid);
        res.payout += opay;
        credits += opay;
        car->parts_value = std::max<int64_t>(0, car->parts_value - bit->price_paid);
        ++res.orphans_removed;
        bit = g.bought.erase(bit);
      } else {
        ++bit;
      }
    }
  } else if (was_fitted && is_aspiration_enum(part_enum) && stock_part_id < 0) {
    // Reverted aftermarket forced induction to Naturally Aspirated: remove dependent Intercooler.
    car->slots["Intercooler"] = -1;
    bool intercooler_valid = false;
    if (db && db->is_open()) {
      std::vector<cars::PartRow> new_opts;
      if (db->options(car->car_id, fitted(*car), new_opts, nullptr)) {
        for (const auto& o : new_opts)
          if (o.part_enum == 29 && o.is_stock) {
            intercooler_valid = true;
            car->slots["Intercooler"] = o.id;
            break;
          }
      }
    }
    if (!intercooler_valid) {
      for (auto bit = g.bought.begin(); bit != g.bought.end();) {
        if (bit->garage_id == garage_id && bit->part_enum == 29) {
          const int64_t opay = sell_part_value(bit->price_paid);
          res.payout += opay;
          credits += opay;
          car->parts_value = std::max<int64_t>(0, car->parts_value - bit->price_paid);
          ++res.orphans_removed;
          bit = g.bought.erase(bit);
        } else {
          ++bit;
        }
      }
    }
  }

  res.ok = true;
  return res;
}

int class_of(double raw, const std::vector<ClassBand>& bands) {
  for (size_t k = 0; k < bands.size(); ++k)
    if (bands[k].raw_max >= raw) return int(k);
  return bands.empty() ? 0 : int(bands.size()) - 1;
}

int shown_index(double raw, const std::vector<ClassBand>& bands) {
  if (bands.empty()) return 0;
  const int k = class_of(raw, bands);
  const double r0 = k ? bands[size_t(k - 1)].raw_max : 0, d0 = k ? bands[size_t(k - 1)].shown_max : 0;
  const double r1 = bands[size_t(k)].raw_max, d1 = bands[size_t(k)].shown_max;
  const double shown = r1 > r0 ? d0 + (raw - r0) / (r1 - r0) * (d1 - d0) : d1;
  // Rounded up; an exact integer stays (to 1e-6).
  return int(std::ceil(shown - 1e-6));
}

bool load_class_bands(const cars::PartsDb& db, std::vector<ClassBand>& out, std::string* error) {
  out.clear();
  if (!db.is_open()) {
    if (error) *error = "database not open";
    return false;
  }
  static const char* kNames[] = {"F", "E", "D", "C", "B", "A", "S", "R3", "R2", "R1", "X"};
  db.each("SELECT Id, MaxPerformanceIndex, MaxDisplayPerformanceIndex FROM CarClasses ORDER BY Id", {},
          [&](sqlite3_stmt* st) {
            const int id = sqlite3_column_int(st, 0);
            ClassBand b{sqlite3_column_double(st, 1), sqlite3_column_double(st, 2),
                        id >= 0 && id < 11 ? kNames[id] : "?"};
            out.push_back(b);
          },
          error);
  if (out.empty() && error && error->empty()) *error = "merged db: no CarClasses rows";
  return !out.empty();
}

bool load_class_bands(const std::filesystem::path& game_root, std::vector<ClassBand>& out, std::string* error,
                      const std::filesystem::path& state_root, const cars::PartsDb* shared_db) {
  out.clear();
  if (shared_db && shared_db->is_open()) {
    return load_class_bands(*shared_db, out, error);
  }
  // Prefer the merged TU4 / DLC / mod database when available.
  std::filesystem::path eff_state = state_root;
  if (eff_state.empty()) {
    if (const char* env = std::getenv("SHIFT_STATE_ROOT"); env && *env) eff_state = env;
  }
  if (!game_root.empty()) {
    cars::Install install;
    std::string why;
    if (cars::discover_install(game_root, eff_state, "", install, &why) &&
        (!install.v4_media_zip.empty() || !install.packages.empty() || !cars::mods::active().empty())) {
      cars::PartsDb merged;
      if (merged.open(install, &why) && load_class_bands(merged, out, nullptr)) {
        return true;
      }
    }
  }
  const auto media = std::filesystem::is_directory(game_root / "media") ? game_root / "media" : game_root;
  const std::string path = cars::mods::resolve(media / "db" / "gamedb.slt").string();
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (error) *error = "cannot open " + path;
    sqlite3_close(db);
    return false;
  }
  sqlite3_stmt* st = nullptr;
  static const char* kNames[] = {"F", "E", "D", "C", "B", "A", "S", "R3", "R2", "R1", "X"};
  if (sqlite3_prepare_v2(db, "SELECT Id, MaxPerformanceIndex, MaxDisplayPerformanceIndex FROM CarClasses ORDER BY Id",
                         -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const int id = sqlite3_column_int(st, 0);
      ClassBand b{sqlite3_column_double(st, 1), sqlite3_column_double(st, 2), id >= 0 && id < 11 ? kNames[id] : "?"};
      out.push_back(b);
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  if (out.empty() && error) *error = path + ": no CarClasses rows";
  return !out.empty();
}

std::string to_text(const Garage& g) {
  std::ostringstream o;
  o.precision(9);
  o << "shift-garage 2\n";
  o << "last " << g.last_id << "\n";
  o << "selected " << g.selected << "\n";
  for (const auto& c : g.cars) {
    o << "car " << c.id << " " << c.car_id << " " << c.performance_index << " " << c.class_id << " " << c.parts_value
      << " " << c.colour;
    for (const auto& [k, v] : c.slots) {
      if (v >= 0) {
        o << " " << k << "=" << v;
      } else if (k == "SingleTurbo" || k == "TwinTurbo" || k == "SuperchargerCSC" || k == "SuperchargerDSC" ||
                 k == "Intercooler") {
        o << " " << k << "=-1";
      }
    }
    o << "\n";
    if (c.favorite || c.is_prize) {
      o << "flags " << c.id;
      if (c.favorite) o << " favorite=1";
      if (c.is_prize) o << " prize=1";
      o << "\n";
    }
    if (!c.tune.is_default()) {
      const std::string tt = tune_to_tokens(c.tune);
      if (!tt.empty()) o << "tune " << c.id << " " << tt << "\n";
    }
    if (!c.livery_file.empty()) {
      o << "livery " << c.id << " " << c.livery_file << "\n";
    }
  }
  if (!g.club_shared_garage_ids.empty()) {
    o << "club_shared";
    for (int64_t gid : g.club_shared_garage_ids) o << " " << gid;
    o << "\n";
  }
  if (g.club_borrowed_garage_id > 0) {
    o << "club_borrowed " << g.club_borrowed_garage_id << " " << g.club_return_garage_id << "\n";
  }
  for (const auto& b : g.bought)
    o << "bought " << b.garage_id << " " << b.part_enum << " " << b.part_id << " " << b.manufacturer << " "
      << b.price_paid << "\n";
  return o.str();
}

bool from_text(const std::string& text, Garage& out, std::string* error) {
  out = Garage{};
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("shift-garage ", 0) != 0) {
    if (error) *error = "not a garage file";
    return false;
  }
  if (std::atoi(line.c_str() + 13) > 2) {
    if (error) *error = "garage file from a newer version";
    return false;
  }
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string key;
    l >> key;
    if (key == "last") {
      l >> out.last_id;
    } else if (key == "selected") {
      l >> out.selected;
    } else if (key == "car") {
      Car c;
      if (!(l >> c.id >> c.car_id >> c.performance_index >> c.class_id >> c.parts_value >> c.colour)) continue;
      for (std::string kv; l >> kv;) {
        const auto eq = kv.find('=');
        if (eq != std::string::npos) c.slots[kv.substr(0, eq)] = std::atoll(kv.c_str() + eq + 1);
      }
      out.cars.push_back(std::move(c));
    } else if (key == "flags") {
      int64_t gid = 0;
      if (l >> gid) {
        if (Car* c = out.find(gid)) {
          for (std::string kv; l >> kv;) {
            if (kv == "favorite=1") c->favorite = true;
            else if (kv == "favorite=0") c->favorite = false;
            else if (kv == "prize=1") c->is_prize = true;
            else if (kv == "prize=0") c->is_prize = false;
          }
        }
      }
    } else if (key == "club_shared") {
      for (int64_t gid = 0; l >> gid;) {
        if (gid > 0) out.club_shared_garage_ids.push_back(gid);
      }
    } else if (key == "club_borrowed") {
      int64_t bg = 0, rg = 0;
      if (l >> bg) {
        out.club_borrowed_garage_id = bg;
        if (l >> rg) out.club_return_garage_id = rg;
      }
    } else if (key == "tune") {
      int64_t gid = 0;
      if (l >> gid) {
        if (Car* c = out.find(gid)) {
          for (std::string kv; l >> kv;) parse_tune_token(kv, c->tune);
        }
      }
    } else if (key == "livery") {
      int64_t gid = 0;
      std::string path;
      if (l >> gid >> path) {
        if (Car* c = out.find(gid)) c->livery_file = path;
      }
    } else if (key == "bought") {
      Bought b;
      if (l >> b.garage_id >> b.part_enum >> b.part_id >> b.manufacturer >> b.price_paid) out.bought.push_back(b);
    }
  }
  return true;
}

bool read_garage(const std::filesystem::path& path, Garage& out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) error->clear();
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return from_text(ss.str(), out, error);
}

bool write_garage(const std::filesystem::path& path, const Garage& g, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out << to_text(g);
    if (!out) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    if (error) *error = "cannot replace " + path.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace garage
