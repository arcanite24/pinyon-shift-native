#include "garage/garage.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace garage {

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
  for (const auto& b : bought)
    if (b.garage_id == garage_id && b.part_enum == part_enum && b.part_id == part_id) return true;
  return false;
}

cars::FittedParts fitted(const Car& car) {
  cars::FittedParts f;
  for (const auto& s : cars::upgrade_slots()) {
    const auto it = car.slots.find(s.column);
    if (it != car.slots.end() && it->second >= 0) f[s.table] = it->second;
  }
  return f;
}

int64_t discounted_price(int64_t price, int discount_percent) {
  if (discount_percent <= 0) return price;
  const float d = float(std::min(discount_percent, 100)) / 100.0f;
  return int64_t(std::floor((1.0f - d) * float(price) + 0.5f));
}

FitResult fit_part(Garage& g, int64_t garage_id, const cars::PartRow& part, const char* column, int discount_percent,
                   int64_t& credits) {
  FitResult r;
  Car* car = g.find(garage_id);
  if (!car) return r;
  // A stock row and a part bought before are refitted for free.
  if (part.is_stock || g.owns_part(garage_id, part.part_enum, part.id)) {
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

bool load_class_bands(const std::filesystem::path& game_root, std::vector<ClassBand>& out, std::string* error) {
  out.clear();
  const auto media = std::filesystem::is_directory(game_root / "media") ? game_root / "media" : game_root;
  const std::string path = (media / "db" / "gamedb.slt").string();
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
  o << "shift-garage 1\n";
  o << "last " << g.last_id << "\n";
  o << "selected " << g.selected << "\n";
  for (const auto& c : g.cars) {
    o << "car " << c.id << " " << c.car_id << " " << c.performance_index << " " << c.class_id << " " << c.parts_value
      << " " << c.colour;
    for (const auto& [k, v] : c.slots)
      if (v >= 0) o << " " << k << "=" << v;
    o << "\n";
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
  if (std::atoi(line.c_str() + 13) > 1) {
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
