#include "cars/database.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
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

bool load_car_physics(const Install& install, const std::vector<int64_t>& ids, std::vector<CarPhysics>& out,
                      std::string* error) {
  out.clear();
  Merged merged;
  if (!open_merged(install, merged, error)) return false;
  sqlite3* db = merged.db;
  for (int64_t id : ids) {
    CarPhysics car;
    car.car_id = id;
    auto& v = car.values;
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
      row_into(db, std::string("SELECT * FROM ") + t.name + " WHERE Ordinal = ? AND IsStock = 1", id, t.name, v,
               &rows);
      if (t.required && rows != 1) return fail(std::string(t.name) + ": " + std::to_string(rows) + " stock rows");
    }
    const auto key = [&](const std::string& k) { return int64_t(std::llround(car.get(k, -1))); };
    const int64_t body = key("List_UpgradeCarBody.CarBodyID");
    row_into(db, "SELECT * FROM Data_CarBody WHERE Id = ?", body, "Data_CarBody", v);
    if (!row_into(db, "SELECT * FROM List_UpgradeCarBodyWeight WHERE CarBodyId = ? AND IsStock = 1", body,
                  "List_UpgradeCarBodyWeight", v))
      return fail("no stock List_UpgradeCarBodyWeight row");
    row_into(db, "SELECT * FROM List_UpgradeCarBodyChassisStiffness WHERE CarBodyId = ? AND IsStock = 1", body,
             "List_UpgradeCarBodyChassisStiffness", v);
    const int64_t engine = key("List_UpgradeEngine.EngineID");
    row_into(db, "SELECT * FROM Data_Engine WHERE EngineID = ?", engine, "Data_Engine", v);
    if (!row_into(db, "SELECT * FROM List_UpgradeEngineCamshaft WHERE EngineID = ? AND IsStock = 1", engine,
                  "List_UpgradeEngineCamshaft", v))
      return fail("no stock camshaft");
    for (const char* t : {"List_UpgradeEngineFlywheel", "List_UpgradeEngineTurboSingle", "List_UpgradeEngineTurboTwin",
                          "List_UpgradeEngineDSC", "List_UpgradeEngineCSC"})
      row_into(db, std::string("SELECT * FROM ") + t + " WHERE EngineID = ? AND IsStock = 1", engine, t, v);
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
      if (!row_into(db, std::string("SELECT * FROM ") + t + " WHERE DrivetrainID = ? AND IsStock = 1", drivetrain, t,
                    v))
        return fail(std::string("no stock ") + t);
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
    out.push_back(std::move(car));
  }
  return true;
}

}  // namespace cars
