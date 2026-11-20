#include "cars/database.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include <sqlite3.h>

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

 private:
  sqlite3_stmt* st_ = nullptr;
};

}  // namespace

bool load_database(const Install& install, Database& out, std::string* error) {
  out = Database{};
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
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_MEMORY, nullptr) != SQLITE_OK) {
    if (error) *error = "sqlite3_open_v2 failed";
    return false;
  }
  struct Closer {
    sqlite3* db;
    ~Closer() { sqlite3_close(db); }
  } closer{db};
  if (!deserialize(db, "main", v4_bytes, error)) return false;
  if (!exec(db, "ATTACH DATABASE ':memory:' AS base", error) ||
      !deserialize(db, "base", base_bytes, error))
    return false;
  const std::set<int64_t> base_ids = car_ids(db, "base");
  const std::set<int64_t> v4_ids = car_ids(db, "main");
  out.base_rows = base_ids.size();
  out.v4_rows = v4_ids.size();
  if (!exec(db, "DETACH DATABASE base", error)) return false;

  std::map<int64_t, std::string> added_by;
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
          for (int64_t id : car_ids(db, "m")) added_by[id] = p.id;
      }
      if (!exec(db, "DETACH DATABASE m", error)) return false;
      out.merges_applied.push_back(p.id + ":" + (member.empty() ? where.filename().string() : member));
    }
  }

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
          "WingMask FROM Data_Car ORDER BY Id");
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

}  // namespace cars
