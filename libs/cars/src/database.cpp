#include "cars/database.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

#include <sqlite3.h>

#include "cars/physics.h"

namespace cars {
namespace fs = std::filesystem;
namespace {

bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool read_member(const fs::path& archive_path, const std::string& member, std::vector<uint8_t>& out,
                 std::string* error) {
  if (member.empty()) {
    if (!read_file(archive_path, out)) {
      if (error) *error = "cannot read " + archive_path.string();
      return false;
    }
    return true;
  }
  auto archive = fh1::Archive::open(archive_path, error);
  if (!archive) return false;
  auto r = archive->read(member);
  if (!r.ok()) {
    if (error) *error = archive_path.string() + "!" + member + ": " + r.error;
    return false;
  }
  out = std::move(r.data);
  return true;
}

// Loads bytes as an in-memory database under `schema` ("main" or an
// attached name). sqlite takes ownership of a copy.
bool deserialize(sqlite3* db, const char* schema, const std::vector<uint8_t>& bytes, std::string* error) {
  auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
  if (!copy) return false;
  std::memcpy(copy, bytes.data(), bytes.size());
  const int rc = sqlite3_deserialize(db, schema, copy, sqlite3_int64(bytes.size()),
                                     sqlite3_int64(bytes.size()),
                                     SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
  if (rc != SQLITE_OK) {
    if (error) *error = std::string("sqlite3_deserialize: ") + sqlite3_errmsg(db);
    return false;
  }
  return true;
}

bool exec(sqlite3* db, const std::string& sql, std::string* error) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    if (error) *error = sql.substr(0, 80) + ": " + (message ? message : "?");
    sqlite3_free(message);
    return false;
  }
  return true;
}

std::string quote(const std::string& id) {
  std::string out = "\"";
  for (char c : id) out += c == '"' ? std::string("\"\"") : std::string(1, c);
  return out + "\"";
}

std::vector<std::string> columns(sqlite3* db, const std::string& schema, const std::string& table) {
  std::vector<std::string> out;
  sqlite3_stmt* st = nullptr;
  const std::string sql = "PRAGMA " + quote(schema) + ".table_info(" + quote(table) + ")";
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW)
    out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
  sqlite3_finalize(st);
  return out;
}

std::vector<std::string> tables(sqlite3* db, const std::string& schema) {
  std::vector<std::string> out;
  sqlite3_stmt* st = nullptr;
  const std::string sql = "SELECT name FROM " + quote(schema) + ".sqlite_master WHERE type='table' ORDER BY name";
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW)
    out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  sqlite3_finalize(st);
  return out;
}

std::set<int64_t> car_ids(sqlite3* db, const char* schema) {
  std::set<int64_t> out;
  sqlite3_stmt* st = nullptr;
  const std::string sql = std::string("SELECT Id FROM ") + schema + ".Data_Car";
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) out.insert(sqlite3_column_int64(st, 0));
  sqlite3_finalize(st);
  return out;
}

class Query {
 public:
  Query(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st_, nullptr) != SQLITE_OK) st_ = nullptr;
  }
  ~Query() { sqlite3_finalize(st_); }
  bool ok() const { return st_ != nullptr; }
  bool step() { return st_ && sqlite3_step(st_) == SQLITE_ROW; }
  int64_t i(int c) const { return sqlite3_column_int64(st_, c); }
  double d(int c) const { return sqlite3_column_double(st_, c); }
  std::string s(int c) const {
    const auto* t = sqlite3_column_text(st_, c);
    return t ? reinterpret_cast<const char*>(t) : "";
  }
  bool null(int c) const { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
  int count() const { return st_ ? sqlite3_column_count(st_) : 0; }
  std::string name(int c) const { return sqlite3_column_name(st_, c); }
  int type(int c) const { return sqlite3_column_type(st_, c); }
  void bind(int index, int64_t v) { sqlite3_bind_int64(st_, index, v); }

 private:
  sqlite3_stmt* st_ = nullptr;
};

// The merged database: v4's (or the disc's) with every package's merge
// databases applied as whole rows by key, in package priority order.
struct Merged {
  sqlite3* db = nullptr;
  std::set<int64_t> base_ids, v4_ids;
  std::map<int64_t, std::string> added_by;
  std::vector<std::string> merges_applied;
  ~Merged() { sqlite3_close(db); }
};

bool open_merged(const Install& install, Merged& m, std::string* error) {
  std::vector<uint8_t> base_bytes, v4_bytes;
  if (!read_file(install.media / "db" / "gamedb.slt", base_bytes)) {
    if (error) *error = "cannot read " + (install.media / "db" / "gamedb.slt").string();
    return false;
  }
  if (!install.v4_media_zip.empty()) {
    if (!read_member(install.v4_media_zip, "db/gamedb.slt", v4_bytes, error)) return false;
  } else {
    v4_bytes = base_bytes;
  }
  if (sqlite3_open_v2(":memory:", &m.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_MEMORY, nullptr) != SQLITE_OK) {
    if (error) *error = "sqlite3_open_v2 failed";
    return false;
  }
  sqlite3* db = m.db;
  if (!deserialize(db, "main", v4_bytes, error)) return false;
  if (!exec(db, "ATTACH DATABASE ':memory:' AS base", error) ||
      !deserialize(db, "base", base_bytes, error))
    return false;
  m.base_ids = car_ids(db, "base");
  m.v4_ids = car_ids(db, "main");
  if (!exec(db, "DETACH DATABASE base", error)) return false;

  const auto main_tables = tables(db, "main");
  for (const Package& p : install.packages) {
    for (const auto& [where, member] : p.merges) {
      std::vector<uint8_t> bytes;
      if (!read_member(where, member, bytes, error)) return false;
      if (!exec(db, "ATTACH DATABASE ':memory:' AS m", error) || !deserialize(db, "m", bytes, error))
        return false;
      for (const std::string& table : tables(db, "m")) {
        if (!std::binary_search(main_tables.begin(), main_tables.end(), table)) continue;
        const auto ours = columns(db, "main", table);
        std::string list;
        for (const std::string& c : columns(db, "m", table)) {
          if (std::find(ours.begin(), ours.end(), c) == ours.end()) continue;
          list += (list.empty() ? "" : ",") + quote(c);
        }
        if (list.empty()) continue;
        if (!exec(db, "INSERT OR REPLACE INTO main." + quote(table) + " (" + list + ") SELECT " + list +
                          " FROM m." + quote(table),
                  error))
          return false;
        if (table == "Data_Car")
          for (int64_t id : car_ids(db, "m")) m.added_by[id] = p.id;
      }
      if (!exec(db, "DETACH DATABASE m", error)) return false;
      m.merges_applied.push_back(p.id + ":" + (member.empty() ? where.filename().string() : member));
    }
  }
  return true;
}

}  // namespace

bool load_database(const Install& install, Database& out, std::string* error) {
  out = Database{};
  Merged merged;
  if (!open_merged(install, merged, error)) return false;
  sqlite3* db = merged.db;
  const std::set<int64_t>& base_ids = merged.base_ids;
  const std::set<int64_t>& v4_ids = merged.v4_ids;
  const std::map<int64_t, std::string>& added_by = merged.added_by;
  out.base_rows = base_ids.size();
  out.v4_rows = v4_ids.size();
  out.merges_applied = merged.merges_applied;

  std::map<int64_t, std::string> wheels;
  {
    Query q(db, "SELECT ID, MediaName FROM List_Wheels");
    while (q.step()) wheels[q.i(0)] = q.s(1);
  }
  Query q(db,
          "SELECT Id, MediaName, DisplayName, StockWheelID, FrontTireWidthMM, FrontTireAspect, "
          "FrontWheelDiameterIN, RearTireWidthMM, RearTireAspect, RearWheelDiameterIN, "
          "FrontStockRideHeight, RearStockRideHeight, CaliperRGBFront, CaliperRGBRear, "
          "CaliperAngleFront, CaliperAngleRear, IsSelectable, IsDrivable, IsUnicorn, HasLivery, "
          "WingMask, HasRaceLivery FROM Data_Car ORDER BY Id");
  if (!q.ok()) {
    if (error) *error = std::string("Data_Car query: ") + sqlite3_errmsg(db);
    return false;
  }
  while (q.step()) {
    CarRow r;
    r.id = q.i(0);
    r.media_name = q.s(1);
    r.display_name = q.s(2);
    r.stock_wheel_id = q.i(3);
    if (auto it = wheels.find(r.stock_wheel_id); it != wheels.end()) r.wheel_media = it->second;
    r.tyre_width_mm = {q.i(4), q.i(7)};
    r.tyre_aspect = {q.i(5), q.i(8)};
    r.rim_inches = {q.i(6), q.i(9)};
    r.ride_height = {q.d(10), q.d(11)};
    r.caliper_rgb_front = uint32_t(q.i(12));
    r.caliper_rgb_rear = uint32_t(q.i(13));
    r.caliper_angle_front = q.d(14);
    r.caliper_angle_rear = q.d(15);
    r.selectable = q.i(16) != 0;
    r.drivable = q.i(17) != 0;
    r.unicorn = q.i(18) != 0;
    r.has_livery = q.i(19) != 0;
    r.wing_mask = q.i(20);
    r.race_livery = q.i(21) != 0;
    r.origin = base_ids.count(r.id) ? "base" : v4_ids.count(r.id) ? "v4" : "";
    if (auto it = added_by.find(r.id); it != added_by.end() && r.origin.empty()) r.origin = it->second;
    out.cars.push_back(std::move(r));
  }
  std::map<int64_t, size_t> by_id;
  for (size_t i = 0; i < out.cars.size(); ++i) by_id[out.cars[i].id] = i;
  {
    Query c(db,
            "SELECT ID, Ordinal, Sequence, Metallic, RGB, BaseTexture, DamageTexture FROM Combo_Colors "
            "ORDER BY Ordinal, Sequence, ID");
    while (c.step()) {
      auto it = by_id.find(c.i(1));
      if (it == by_id.end()) continue;
      CarColour col;
      col.id = c.i(0);
      col.sequence = c.i(2);
      col.metallic = c.i(3) != 0;
      col.rgb = uint32_t(c.i(4));
      col.base_texture = c.s(5);
      col.damage_texture = c.s(6);
      out.cars[it->second].colours.push_back(std::move(col));
    }
  }
  {
    Query c(db,
            "SELECT Ordinal, Sequence, HasStripe, ColorStripe, HasColor2 FROM Combo_Colors_Exceptions "
            "ORDER BY Ordinal, Sequence, ID");
    std::set<int64_t> cars_with;
    while (c.step()) {
      ++out.exception_rows;
      cars_with.insert(c.i(0));
      out.exception_stripes += c.i(2) != 0;
      out.exception_color2 += c.i(4) != 0;
      auto it = by_id.find(c.i(0));
      if (it == by_id.end() || c.i(2) == 0) continue;
      for (CarColour& col : out.cars[it->second].colours)
        if (col.sequence == c.i(1) && !col.has_stripe) {
          col.has_stripe = true;
          col.stripe_rgb = uint32_t(c.i(3));
        }
    }
    out.exception_cars = cars_with.size();
  }
  {
    Query c(db,
            "SELECT Id, ModelFrontTrackOuter, ModelRearTrackOuter, ModelWheelbase, BottomCenterWheelbasePosY "
            "FROM Data_CarBody ORDER BY Id");
    while (c.step()) {
      if (c.i(0) % 1000 != 0) continue;
      auto it = by_id.find(c.i(0) / 1000);
      if (it == by_id.end()) continue;
      CarRow& r = out.cars[it->second];
      r.has_body = true;
      r.track_outer = {c.d(1), c.d(2)};
      r.wheelbase = c.d(3);
      r.body_bottom_y = c.d(4);
    }
  }
  {
    // FORMAT-CARS, Stock parts: each car's IsStock rows of the upgrade
    // tables, through List_PartsStrings (a comma list; "-seatL" removes).
    std::map<int64_t, std::string> strings;
    {
      Query s(db, "SELECT Id, PartsString FROM List_PartsStrings");
      while (s.step()) strings[s.i(0)] = s.s(1);
    }
    std::map<int64_t, int64_t> body_of;  // car id -> its stock CarBodyID
    {
      Query b(db, "SELECT Ordinal, CarBodyID FROM List_UpgradeCarBody WHERE IsStock = 1 ORDER BY rowid");
      while (b.step()) body_of.emplace(b.i(0), b.i(1));
    }
    std::map<int64_t, std::vector<int64_t>> by_body, by_car;  // string ids
    struct Table {
      const char* name;
      const char* key;
      const char* column;
      bool by_car;
    };
    static constexpr Table kTables[] = {
        {"List_UpgradeCarBodyFrontBumper", "CarBodyID", "PartsStringId", false},
        {"List_UpgradeCarBodyRearBumper", "CarBodyID", "PartsStringId", false},
        {"List_UpgradeCarBodySideSkirt", "CarBodyID", "PartsStringId", false},
        {"List_UpgradeCarBodyHood", "CarBodyID", "PartsStringId", false},
        {"List_UpgradeRearWing", "Ordinal", "PartsStringId", true},
        {"List_UpgradeCarBodyChassisStiffness", "CarbodyId", "PartsStringID", false},
        {"List_UpgradeCarBodyWeight", "CarBodyId", "PartsStringID", false},
    };
    for (const Table& t : kTables) {
      Query u(db, std::string("SELECT ") + t.key + ", " + t.column + " FROM " + t.name + " WHERE IsStock = 1");
      while (u.step()) {
        if (u.null(1)) continue;
        const int64_t id = u.i(1);
        if (id == 0) continue;  // none
        (t.by_car ? by_car : by_body)[u.i(0)].push_back(id);
      }
    }
    for (CarRow& r : out.cars) {
      std::vector<int64_t> ids = by_car[r.id];
      if (auto b = body_of.find(r.id); b != body_of.end()) {
        r.has_stock_body = true;
        const auto& more = by_body[b->second];
        ids.insert(ids.end(), more.begin(), more.end());
      }
      std::set<std::string> shown, removed;
      for (int64_t id : ids) {
        auto s = strings.find(id);
        if (s == strings.end()) continue;
        std::string list = s->second;
        size_t at = 0;
        while (at <= list.size()) {
          size_t end = list.find(',', at);
          if (end == std::string::npos) end = list.size();
          std::string name = list.substr(at, end - at);
          while (!name.empty() && name.front() == ' ') name.erase(name.begin());
          while (!name.empty() && name.back() == ' ') name.pop_back();
          for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
          if (!name.empty() && name[0] == '-') removed.insert(name.substr(1));
          else if (!name.empty()) shown.insert(name);
          at = end + 1;
        }
      }
      r.stock_parts.assign(shown.begin(), shown.end());
      r.removed_parts.assign(removed.begin(), removed.end());
    }
  }
  {
    Query c(db, "SELECT Id FROM CarTrackOffsets");
    while (c.step())
      if (auto it = by_id.find(c.i(0)); it != by_id.end()) out.cars[it->second].has_track_offsets = true;
  }
  {
    // No unique key: the merge can leave identical duplicate rows. The
    // first row by rowid is used; differing duplicates are counted.
    Query c(db, "SELECT Ordinal, PartId, PartPosX, PartPosY, PartPosZ FROM CarPartPositions ORDER BY Ordinal, PartId, rowid");
    while (c.step()) {
      auto it = by_id.find(c.i(0));
      const int64_t part = c.i(1);
      if (it == by_id.end() || part < 0 || part > 9) continue;
      CarRow& r = out.cars[it->second];
      const std::array<double, 3> p{c.d(2), c.d(3), c.d(4)};
      if (r.has_part_position[size_t(part)]) {
        if (r.part_positions[size_t(part)] == p) ++r.part_position_duplicates;
        continue;
      }
      r.part_positions[size_t(part)] = p;
      r.has_part_position[size_t(part)] = true;
    }
  }
  return true;
}

namespace {

// One row into `values` under `group.`, numeric columns only. Returns
// whether a row was found; `rows` counts them (a stock row must be one).
bool row_into(sqlite3* db, const std::string& sql, int64_t key, const std::string& group,
              std::map<std::string, double>& values, int* rows = nullptr) {
  Query q(db, sql);
  if (!q.ok()) return false;
  q.bind(1, key);
  int n = 0;
  while (q.step()) {
    if (n++ == 0)
      for (int c = 0; c < q.count(); ++c)
        if (q.type(c) == SQLITE_INTEGER || q.type(c) == SQLITE_FLOAT) values[group + "." + q.name(c)] = q.d(c);
  }
  if (rows) *rows = n;
  return n > 0;
}

// v0..v(count-1) of the current row, with `count` from the named column;
// the row's other numeric columns go to `values` under `group.` when given.
std::vector<double> samples_of(const Query& q, const char* count_column, const std::string& group,
                               std::map<std::string, double>* values) {
  int n = 0;
  for (int c = 0; c < q.count(); ++c)
    if (q.name(c) == count_column) n = int(q.i(c));
  std::vector<double> samples(size_t(std::max(n, 0)));
  for (int c = 0; c < q.count(); ++c) {
    const std::string name = q.name(c);
    if (name.size() > 1 && name[0] == 'v' && std::isdigit(static_cast<unsigned char>(name[1]))) {
      const int i = std::atoi(name.c_str() + 1);
      if (i < n) samples[size_t(i)] = q.d(c);
    } else if (values && (q.type(c) == SQLITE_INTEGER || q.type(c) == SQLITE_FLOAT)) {
      (*values)[group + "." + name] = q.d(c);
    }
  }
  return samples;
}

}  // namespace

namespace {

// The value of attribute `name` in the first `<tag ...>` element of `xml`.
bool xml_attribute(const std::string& xml, const std::string& tag, const std::string& name, double& out) {
  const size_t at = xml.find("<" + tag);
  if (at == std::string::npos) return false;
  const size_t end = xml.find('>', at);
  const std::string element = xml.substr(at, end == std::string::npos ? std::string::npos : end - at);
  const std::string key = " " + name + "=\"";
  const size_t k = element.find(key);
  if (k == std::string::npos) return false;
  out = std::strtod(element.c_str() + k + key.size(), nullptr);
  return true;
}

}  // namespace

bool load_car_camera(const Install& install, int64_t car_id, const std::string& media, CarCameraRow& out,
                     std::string* error) {
  out = {};
  {
    Merged merged;
    if (!open_merged(install, merged, error)) return false;
    Query q(merged.db, "SELECT * FROM CameraOverrides WHERE CarId = ?");
    if (q.ok()) {
      q.bind(1, car_id);
      if (q.step()) {
        out.has_overrides = true;
        for (int c = 0; c < q.count(); ++c)
          if (q.type(c) == SQLITE_INTEGER || q.type(c) == SQLITE_FLOAT) out.overrides[q.name(c)] = q.d(c);
      }
    }
  }
  const auto ref = install.find_car(lower(media));
  if (!ref) {
    if (error) *error = "no car folder for " + media;
    return false;
  }
  Folder folder;
  std::vector<uint8_t> bytes;
  if (!folder.open(*ref, error) || !folder.read("physics/maxdata.xml", bytes, error)) return false;
  const std::string xml(bytes.begin(), bytes.end());
  const char* names[6] = {"MinX", "MinY", "MinZ", "MaxX", "MaxY", "MaxZ"};
  out.has_box = true;
  for (int i = 0; i < 6; ++i) out.has_box = out.has_box && xml_attribute(xml, "BoundingBox", names[i], out.box[size_t(i)]);
  if (!out.has_box && error) *error = media + ": no Collision/BoundingBox in MAXData.xml";
  return out.has_box;
}

bool load_camera_files(const Install& install, std::string& settings_ini, std::string& physics_xml,
                       std::string* error) {
  std::vector<uint8_t> bytes;
  if (!read_member(install.media / "camera.zip", "CameraSettings.ini", bytes, error)) return false;
  settings_ini.assign(bytes.begin(), bytes.end());
  if (install.v4_media_zip.empty() ||
      !read_member(install.v4_media_zip, "camera/CameraPhysics.xml", bytes, nullptr)) {
    if (!read_member(install.media / "camera.zip", "CameraPhysics.xml", bytes, error)) return false;
  }
  physics_xml.assign(bytes.begin(), bytes.end());
  return true;
}

namespace {

// One car's rows (load_car_physics), with `fit` its fitted parts.
bool car_physics_from(sqlite3* db, int64_t id, const FittedParts& fit, CarPhysics& out, std::string* error) {
  {
    CarPhysics car;
    car.car_id = id;
    auto& v = car.values;
    // The row of table `t`: the fitted one by Id, else the stock one by
    // `where` (SQL with one ? for `key`).
    auto part = [&](const std::string& t, const std::string& where, int64_t key, int* rows = nullptr) {
      const auto f = fit.find(t);
      if (f != fit.end() && f->second >= 0)
        return row_into(db, "SELECT * FROM " + t + " WHERE Id = ?", f->second, t, v, rows);
      return row_into(db, "SELECT * FROM " + t + " WHERE " + where, key, t, v, rows);
    };
    auto fail = [&](const std::string& what) {
      if (error) *error = "car " + std::to_string(id) + ": " + what;
      return false;
    };
    {
      Query q(db, "SELECT MediaName FROM Data_Car WHERE Id = ?");
      q.bind(1, id);
      if (!q.step()) return fail("no Data_Car row");
      car.media_name = q.s(0);
    }
    row_into(db, "SELECT * FROM Data_Car WHERE Id = ?", id, "Data_Car", v);
    // The stock row of each per-car upgrade table (SPEC-CAR-PHYSICS: by
    // IsStock, not by Level). A required table without exactly one fails.
    struct Table {
      const char* name;
      bool required;
    };
    for (const Table t : {Table{"List_UpgradeCarBody", true}, Table{"List_UpgradeEngine", true},
                          Table{"List_UpgradeDrivetrain", true}, Table{"List_UpgradeBrakes", true},
                          Table{"List_UpgradeSpringDamper", true}, Table{"List_UpgradeAntiSwayFront", true},
                          Table{"List_UpgradeAntiSwayRear", true}, Table{"List_UpgradeTireCompound", true},
                          Table{"List_UpgradeRearWing", false}, Table{"List_UpgradeRimSizeFront", false},
                          Table{"List_UpgradeRimSizeRear", false}}) {
      int rows = 0;
      part(t.name, "Ordinal = ? AND IsStock = 1", id, &rows);
      if (t.required && rows != 1) return fail(std::string(t.name) + ": " + std::to_string(rows) + " stock rows");
    }
    const auto key = [&](const std::string& k) { return int64_t(std::llround(car.get(k, -1))); };
    const int64_t body = key("List_UpgradeCarBody.CarBodyID");
    row_into(db, "SELECT * FROM Data_CarBody WHERE Id = ?", body, "Data_CarBody", v);
    if (!part("List_UpgradeCarBodyWeight", "CarBodyId = ? AND IsStock = 1", body))
      return fail("no stock List_UpgradeCarBodyWeight row");
    part("List_UpgradeCarBodyChassisStiffness", "CarBodyId = ? AND IsStock = 1", body);
    const int64_t engine = key("List_UpgradeEngine.EngineID");
    row_into(db, "SELECT * FROM Data_Engine WHERE EngineID = ?", engine, "Data_Engine", v);
    if (!part("List_UpgradeEngineCamshaft", "EngineID = ? AND IsStock = 1", engine))
      return fail("no stock camshaft");
    for (const char* t : {"List_UpgradeEngineFlywheel", "List_UpgradeEngineTurboSingle", "List_UpgradeEngineTurboTwin",
                          "List_UpgradeEngineDSC", "List_UpgradeEngineCSC"})
      part(t, "EngineID = ? AND IsStock = 1", engine);
    {
      Query q(db, "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID = ?");
      q.bind(1, key("List_UpgradeEngineCamshaft.TorqueCurveFullThrottleID"));
      if (!q.step()) return fail("no torque curve");
      car.curves["torque"] = samples_of(q, "NumTorqueValues", "List_TorqueCurve", &v);
    }
    const int64_t drivetrain = key("List_UpgradeDrivetrain.DrivetrainID");
    row_into(db, "SELECT * FROM Data_Drivetrain WHERE DrivetrainID = ?", drivetrain, "Data_Drivetrain", v);
    for (const char* t : {"List_UpgradeDrivetrainTransmission", "List_UpgradeDrivetrainDifferential",
                          "List_UpgradeDrivetrainClutch", "List_UpgradeDrivetrainDriveline"})
      if (!part(t, "DrivetrainID = ? AND IsStock = 1", drivetrain)) return fail(std::string("no stock ") + t);
    for (const std::string a : {"Front", "Rear"}) {
      if (!row_into(db, "SELECT * FROM List_SpringDamperPhysics WHERE SpringDamperPhysicsID = ?",
                    key("List_UpgradeSpringDamper." + a + "SpringDamperPhysicsID"), a + "SpringDamper", v))
        return fail("no " + a + " spring damper");
      if (!row_into(db, "SELECT * FROM List_AntiSwayPhysics WHERE AntiSwayPhysicsID = ?",
                    key("List_UpgradeAntiSway" + a + ".AntiSwayPhysicsID"), a + "AntiSway", v))
        return fail("no " + a + " anti-roll bar");
    }
    if (!row_into(db, "SELECT * FROM List_TireCompound WHERE TireCompoundID = ?",
                  key("List_UpgradeTireCompound.TireCompoundID"), "List_TireCompound", v))
      return fail("no tyre compound");
    struct Dir {
      const char* group;
      const char* column;
      const char* curve;
    };
    for (const Dir d : {Dir{"LatMultiCurve", "List_TireCompound.FrictionMultiCurveLateralID", "lat"},
                        Dir{"LongMultiCurve", "List_TireCompound.FrictionMultiCurveLongitudinalID", "long"}}) {
      if (!row_into(db, "SELECT * FROM List_TireFrictionMultiCurve WHERE FrictionMultiCurveID = ?", key(d.column),
                    d.group, v))
        return fail(std::string("no ") + d.group);
      for (int c = 0; c < 2; ++c) {
        Query q(db, "SELECT * FROM List_TireFrictionCurve WHERE FrictionCurveID = ?");
        q.bind(1, key(std::string(d.group) + ".TireFrictionCurveID" + std::to_string(c)));
        if (!q.step()) return fail(std::string("no friction curve for ") + d.group);
        car.curves[d.curve + std::to_string(c)] = samples_of(q, "NumCurveValues", "", nullptr);
        // The row's own FrictionScale (SPEC-CAR-PHYSICS, Curves).
        for (int col = 0; col < q.count(); ++col)
          if (q.name(col) == "FrictionScale")
            v[std::string(d.group) + ".FrictionScale" + std::to_string(c)] = q.d(col);
      }
    }
    // No fit: the stock row of every slot (the first by Id), so a car whose
    // stock rows are upgrades (the showcase cars: SimPeakPower twice the
    // base model's) gets their mass and torque deltas as a fitted car does.
    FittedParts stock_fit;
    if (fit.empty()) {
      const int64_t keys[4] = {id, engine, drivetrain, body};
      for (const UpgradeSlot& s : upgrade_slots()) {
        const char* col = s.key == 0 ? "Ordinal" : s.key == 1 ? "EngineID" : s.key == 2 ? "DrivetrainID" : "CarBodyID";
        Query q(db, std::string("SELECT Id FROM ") + s.table + " WHERE " + col + " = ? AND IsStock = 1 ORDER BY Id LIMIT 1");
        if (!q.ok()) continue;
        q.bind(1, keys[s.key]);
        if (q.step()) stock_fit[s.table] = q.i(0);
      }
    }
    const FittedParts& eff = fit.empty() ? stock_fit : fit;
    if (!eff.empty()) {
      // The fitted rows the stock chain does not read, then the deltas.
      for (const auto& [t, row] : eff)
        if (row >= 0 && !car.has(t + ".Id")) row_into(db, "SELECT * FROM " + t + " WHERE Id = ?", row, t, v);
      double mass_diff = 0, torque = 1, share = 0;
      for (const auto& [t, row] : eff) {
        if (row < 0 || t == "List_UpgradeCarBodyWeight") continue;
        mass_diff += car.get(t + ".MassDiff", 0);
        share += car.get(t + ".WeightDistDiff", 0);
        static const char* kTorque[] = {"List_UpgradeEngineValves",   "List_UpgradeEngineDisplacement",
                                        "List_UpgradeEnginePistonsCompression", "List_UpgradeEngineFuelSystem",
                                        "List_UpgradeEngineIgnition", "List_UpgradeEngineExhaust",
                                        "List_UpgradeEngineIntake",   "List_UpgradeEngineManifold",
                                        "List_UpgradeEngineOilCooling"};
        for (const char* k : kTorque)
          if (t == k) torque += car.get(t + ".TorqueScale", 1) - 1;  // additive: 1 + the sum of (s - 1) (SPEC-GARAGE-UPGRADES)
      }
      v["List_UpgradeCarBodyWeight.Mass"] = car.get("List_UpgradeCarBodyWeight.Mass", 0) + mass_diff;
      v["List_TorqueCurve.TorqueScale"] = car.get("List_TorqueCurve.TorqueScale", 1) * torque;
      v["List_UpgradeCarBodyWeight.CMBackFront"] = car.get("List_UpgradeCarBodyWeight.CMBackFront", 0.5) + share;
      // CMHeightDiff is unused by the game (1.0 on all 623 rows,
      // SPEC-GARAGE-UPGRADES); the product leaves the height as it is.
      v["List_UpgradeCarBodyWeight.CMHeight"] = car.get("List_UpgradeCarBodyWeight.CMHeight", 0) *
                                               car.get("List_UpgradeCarBodyChassisStiffness.CMHeightDiff", 1);
      for (const std::string a : {"Front", "Rear"}) {
        const std::string w = "List_UpgradeCarBodyTireWidth" + a + "." + a + "TireWidth";
        const std::string r = "List_UpgradeRimSize" + a + "." + a + "WheelDiameter";
        if (fit.count("List_UpgradeCarBodyTireWidth" + a) && car.has(w)) v["Data_Car." + a + "TireWidthMM"] = car.get(w);
        if (fit.count("List_UpgradeRimSize" + a) && car.has(r)) v["Data_Car." + a + "WheelDiameterIN"] = car.get(r);
      }
    }
    out = std::move(car);
  }
  return true;
}

}  // namespace

bool load_car_physics(const Install& install, const std::vector<int64_t>& ids, std::vector<CarPhysics>& out,
                      std::string* error, const std::vector<FittedParts>* fitted) {
  out.clear();
  Merged merged;
  if (!open_merged(install, merged, error)) return false;
  static const FittedParts kNone;
  for (size_t index = 0; index < ids.size(); ++index) {
    CarPhysics car;
    if (!car_physics_from(merged.db, ids[index], fitted && index < fitted->size() ? (*fitted)[index] : kNone, car,
                          error))
      return false;
    out.push_back(std::move(car));
  }
  return true;
}

struct PartsDb::Impl {
  mutable std::mutex mutex;
  Merged merged;
  std::map<int64_t, uint32_t> offers;  // the enabled packages' offer numbers -> licence masks
};

PartsDb::PartsDb() = default;
PartsDb::~PartsDb() = default;

bool PartsDb::open(const Install& install, std::string* error) {
  auto impl = std::make_unique<Impl>();
  if (!open_merged(install, impl->merged, error)) return false;
  for (const auto& p : install.packages)
    if (p.puboffer >= 0) impl->offers[p.puboffer] |= p.licence_mask;
  impl_ = std::move(impl);
  return true;
}

bool PartsDb::physics(int64_t car_id, const FittedParts& fit, CarPhysics& out, std::string* error) const {
  if (!impl_) {
    if (error) *error = "parts database not open";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return car_physics_from(impl_->merged.db, car_id, fit, out, error);
}

bool PartsDb::options(int64_t car_id, const FittedParts& fit, std::vector<PartRow>& out, std::string* error) const {
  out.clear();
  if (!impl_) {
    if (error) *error = "parts database not open";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  sqlite3* db = impl_->merged.db;
  // The car's keys through its fitted (or stock) engine, drivetrain and
  // body rows.
  CarPhysics car;
  if (!car_physics_from(db, car_id, fit, car, error)) return false;
  const int64_t keys[4] = {car_id, int64_t(std::llround(car.get("List_UpgradeEngine.EngineID", -1))),
                           int64_t(std::llround(car.get("List_UpgradeDrivetrain.DrivetrainID", -1))),
                           int64_t(std::llround(car.get("List_UpgradeCarBody.CarBodyID", -1)))};
  for (const UpgradeSlot& slot : upgrade_slots()) {
    const std::string table = slot.table;
    const auto cols = columns(db, "main", table);
    if (cols.empty()) continue;
    auto has = [&](const char* c) {
      for (const auto& k : cols)
        if (std::equal(k.begin(), k.end(), c, c + std::strlen(c), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); })) return true;
      return false;
    };
    std::string key_col;
    for (const char* k : {slot.key == 0 ? "Ordinal" : slot.key == 1 ? "EngineID" : slot.key == 2 ? "DrivetrainID" : "CarBodyID"})
      if (has(k)) key_col = k;
    if (key_col.empty()) continue;
    const std::string sql = "SELECT Id, Level, IsStock, " + std::string(has("Price") ? "Price" : "0") + ", " +
                            (has("ManufacturerID") ? "ManufacturerID" : "0") + " FROM " + quote(table) + " WHERE " +
                            key_col + " = ? ORDER BY Level, Id";
    Query q(db, sql);
    if (!q.ok()) continue;
    q.bind(1, keys[slot.key]);
    while (q.step()) {
      PartRow r;
      r.part_enum = slot.part_enum;
      r.table = table;
      r.id = q.i(0), r.level = int(q.i(1)), r.is_stock = q.i(2) != 0, r.price = q.i(3), r.manufacturer = q.i(4);
      out.push_back(std::move(r));
    }
  }
  return true;
}

bool PartsDb::dealer(std::vector<DealerCar>& out, std::string* error) const {
  out.clear();
  if (!impl_) {
    if (error) *error = "parts database not open";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Query q(impl_->merged.db,
          "SELECT Id, BaseCost, Year, ClassID, PerformanceIndex, MediaName, (SELECT MIN(Sequence) FROM Combo_Colors k "
          "WHERE k.Ordinal = d.Id) FROM Data_Car d WHERE IsDrivable = 1 AND "
          "IsSelectable = 1 AND IsUnicorn = 0 AND EXISTS (SELECT 1 FROM PowerTrains p WHERE p.Id = d.PowertrainID) "
          "ORDER BY ClassID, PerformanceIndex, Id");
  if (!q.ok()) {
    if (error) *error = "Data_Car: the dealer query failed";
    return false;
  }
  // SPEC-AUTOSHOW-PAINT, Which cars are listed, 4: a row no offer maps is
  // visible; a mapped row is visible when one of its offers is owned, that
  // is its publication's package is installed and the offer's mask is 0 or
  // shares a bit with the package's licence mask (SPEC-V4-CONTENT).
  std::map<int64_t, bool> visible;  // car id -> owned through some offer
  {
    Query m(impl_->merged.db,
            "SELECT m.ContentId, o.PubOfferingID, o.LicenseMask FROM ContentOffersMapping m JOIN ContentOffers o ON "
            "o.OfferId = m.OfferId WHERE m.ContentType = 1");
    while (m.ok() && m.step()) {
      const auto pkg = impl_->offers.find(m.i(1));
      const uint32_t mask = uint32_t(m.i(2));
      const bool owned = pkg != impl_->offers.end() && (mask == 0 || (mask & pkg->second) != 0);
      bool& v = visible.try_emplace(m.i(0), false).first->second;
      v = v || owned;
    }
  }
  while (q.step()) {
    DealerCar c;
    c.id = q.i(0), c.base_cost = q.i(1), c.year = q.i(2), c.class_id = q.i(3), c.performance_index = q.d(4);
    if (const auto it = visible.find(c.id); it != visible.end() && !it->second) continue;
    c.media_name = q.s(5);
    c.first_colour = q.type(6) == SQLITE_NULL ? 0 : q.i(6);
    out.push_back(std::move(c));
  }
  return true;
}

bool PartsDb::stock_slots(int64_t car_id, std::map<std::string, int64_t>& out, std::string* error) const {
  out.clear();
  std::vector<PartRow> rows;
  if (!options(car_id, {}, rows, error)) return false;
  for (const UpgradeSlot& s : upgrade_slots())
    for (const PartRow& r : rows)
      if (r.part_enum == s.part_enum && r.is_stock) {
        auto it = out.find(s.column);
        if (it == out.end() || r.id < it->second) out[s.column] = r.id;
      }
  return true;
}

bool PartsDb::each(const std::string& sql, const std::vector<std::string>& params,
                   const std::function<void(sqlite3_stmt*)>& row, std::string* error) const {
  if (!impl_) {
    if (error) *error = "parts database not open";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  sqlite3* db = impl_->merged.db;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (error) *error = std::string("gamedb: ") + sqlite3_errmsg(db);
    return false;
  }
  for (size_t i = 0; i < params.size(); ++i)
    sqlite3_bind_text(st, int(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) row(st);
  const bool ok = rc == SQLITE_DONE;
  if (!ok && error) *error = std::string("gamedb: ") + sqlite3_errmsg(db);
  sqlite3_finalize(st);
  return ok;
}

const std::vector<UpgradeSlot>& upgrade_slots() {
  // SPEC-GARAGE-UPGRADES, Areas, slots and tables: the part enum, the
  // Career_Garage column, the table and its key (0 car, 1 engine,
  // 2 drivetrain, 3 body). The empty motor families are left out.
  static const std::vector<UpgradeSlot> slots = {
      {0, "Engine", "List_UpgradeEngine", 0},
      {1, "Drivetrain", "List_UpgradeDrivetrain", 0},
      {2, "CarBody", "List_UpgradeCarBody", 0},
      {4, "Brakes", "List_UpgradeBrakes", 0},
      {5, "SpringDamper", "List_UpgradeSpringDamper", 0},
      {6, "AntiSwayFront", "List_UpgradeAntiSwayFront", 0},
      {7, "AntiSwayRear", "List_UpgradeAntiSwayRear", 0},
      {8, "TireCompound", "List_UpgradeTireCompound", 0},
      {9, "RearWing", "List_UpgradeRearWing", 0},
      {10, "RimSizeFront", "List_UpgradeRimSizeFront", 0},
      {11, "RimSizeRear", "List_UpgradeRimSizeRear", 0},
      {12, "Camshaft", "List_UpgradeEngineCamshaft", 1},
      {13, "Valves", "List_UpgradeEngineValves", 1},
      {14, "Displacement", "List_UpgradeEngineDisplacement", 1},
      {15, "PistonsCompression", "List_UpgradeEnginePistonsCompression", 1},
      {16, "FuelSystem", "List_UpgradeEngineFuelSystem", 1},
      {17, "Ignition", "List_UpgradeEngineIgnition", 1},
      {18, "Exhaust", "List_UpgradeEngineExhaust", 1},
      {19, "Intake", "List_UpgradeEngineIntake", 1},
      {20, "Flywheel", "List_UpgradeEngineFlywheel", 1},
      {21, "Manifold", "List_UpgradeEngineManifold", 1},
      {22, "RestrictorPlate", "List_UpgradeEngineRestrictorPlate", 1},
      {23, "OilCooling", "List_UpgradeEngineOilCooling", 1},
      {24, "SingleTurbo", "List_UpgradeEngineTurboSingle", 1},
      {25, "TwinTurbo", "List_UpgradeEngineTurboTwin", 1},
      {27, "SuperchargerCSC", "List_UpgradeEngineCSC", 1},
      {28, "SuperchargerDSC", "List_UpgradeEngineDSC", 1},
      {29, "Intercooler", "List_UpgradeEngineIntercooler", 1},
      {30, "Clutch", "List_UpgradeDrivetrainClutch", 2},
      {31, "Transmission", "List_UpgradeDrivetrainTransmission", 2},
      {32, "Driveline", "List_UpgradeDrivetrainDriveline", 2},
      {33, "Differential", "List_UpgradeDrivetrainDifferential", 2},
      {34, "FrontBumper", "List_UpgradeCarBodyFrontBumper", 3},
      {35, "RearBumper", "List_UpgradeCarBodyRearBumper", 3},
      {36, "Hood", "List_UpgradeCarBodyHood", 3},
      {37, "SideSkirts", "List_UpgradeCarBodySideSkirt", 3},
      {38, "TireWidthFront", "List_UpgradeCarBodyTireWidthFront", 3},
      {39, "TireWidthRear", "List_UpgradeCarBodyTireWidthRear", 3},
      {40, "WeightReduction", "List_UpgradeCarBodyWeight", 3},
      {41, "ChassisStiffness", "List_UpgradeCarBodyChassisStiffness", 3},
  };
  return slots;
}

}  // namespace cars
