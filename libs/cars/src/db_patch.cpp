#include "cars/db_patch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <system_error>

#include <sqlite3.h>

namespace cars::db_patch {
namespace fs = std::filesystem;
namespace {

std::mutex g_mutex;
std::vector<Patch> g_active;
Report g_last;

std::string lower_ascii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

std::string quote_id(const std::string& id) {
  std::string out = "\"";
  for (char c : id) out += c == '"' ? std::string("\"\"") : std::string(1, c);
  return out + "\"";
}

bool exec(sqlite3* db, const std::string& sql, std::string* error) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    if (error) *error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    return false;
  }
  return true;
}

std::vector<std::string> list(sqlite3* db, const std::string& sql, int column = 0) {
  std::vector<std::string> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const auto* t = sqlite3_column_text(st, column);
    out.emplace_back(t ? reinterpret_cast<const char*>(t) : "");
  }
  sqlite3_finalize(st);
  return out;
}

std::set<int64_t> car_ids(sqlite3* db) {
  std::set<int64_t> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT Id FROM main.Data_Car", -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) out.insert(sqlite3_column_int64(st, 0));
  sqlite3_finalize(st);
  return out;
}

// Mod scripts may read and write the main schema, but not reach files.
int authorizer(void* allow_attach, int action, const char*, const char*, const char*, const char*) {
  if ((action == SQLITE_ATTACH || action == SQLITE_DETACH) && !*static_cast<bool*>(allow_attach)) return SQLITE_DENY;
  return SQLITE_OK;
}

bool deserialize(sqlite3* db, const char* schema, const std::vector<uint8_t>& bytes, std::string* error) {
  auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
  if (!copy) return false;
  std::memcpy(copy, bytes.data(), bytes.size());
  if (sqlite3_deserialize(db, schema, copy, sqlite3_int64(bytes.size()), sqlite3_int64(bytes.size()),
                          SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE) != SQLITE_OK) {
    if (error) *error = std::string("not a database: ") + sqlite3_errmsg(db);
    return false;
  }
  return true;
}

// A merge database's rows over main, by key, for the tables main has. The
// rows go in under a savepoint (ATTACH cannot run inside one).
bool merge_rows(sqlite3* db, const std::vector<uint8_t>& bytes, std::string* error) {
  if (!exec(db, "ATTACH DATABASE ':memory:' AS modmerge", error)) return false;
  bool ok = deserialize(db, "modmerge", bytes, error) && exec(db, "SAVEPOINT modmergerows", error);
  if (ok) {
    const auto main_tables = list(db, "SELECT name FROM main.sqlite_master WHERE type='table'");
    for (const std::string& table : list(db, "SELECT name FROM modmerge.sqlite_master WHERE type='table'")) {
      if (std::find(main_tables.begin(), main_tables.end(), table) == main_tables.end()) continue;
      const auto ours = list(db, "PRAGMA main.table_info(" + quote_id(table) + ")", 1);
      std::string cols;
      for (const std::string& c : list(db, "PRAGMA modmerge.table_info(" + quote_id(table) + ")", 1))
        if (std::find(ours.begin(), ours.end(), c) != ours.end()) cols += (cols.empty() ? "" : ",") + quote_id(c);
      if (cols.empty()) continue;
      if (!exec(db, "INSERT OR REPLACE INTO main." + quote_id(table) + " (" + cols + ") SELECT " + cols +
                        " FROM modmerge." + quote_id(table),
                error)) {
        ok = false;
        break;
      }
    }
    std::string ignore;
    if (!ok) exec(db, "ROLLBACK TO modmergerows", &ignore);
    exec(db, "RELEASE modmergerows", &ignore);
  }
  std::string ignore;
  exec(db, "DETACH DATABASE modmerge", &ignore);
  return ok;
}

}  // namespace

std::vector<Patch> collect(const std::vector<std::pair<std::string, fs::path>>& mods_winner_first) {
  std::vector<Patch> out;
  std::error_code ec;
  for (auto it = mods_winner_first.rbegin(); it != mods_winner_first.rend(); ++it) {
    const fs::path db = it->second / "db";
    if (!fs::is_directory(db, ec)) continue;
    std::vector<Patch> mine;
    for (const auto& e : fs::directory_iterator(db, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string ext = lower_ascii(e.path().extension().string());
      if (ext != ".sql" && ext != ".slt") continue;
      mine.push_back(Patch{it->first, e.path(), ext == ".slt"});
    }
    std::sort(mine.begin(), mine.end(), [](const Patch& a, const Patch& b) {
      return lower_ascii(a.file.filename().string()) < lower_ascii(b.file.filename().string());
    });
    out.insert(out.end(), mine.begin(), mine.end());
  }
  return out;
}

bool apply(sqlite3* db, const std::vector<Patch>& patches, Report& report) {
  if (!db) return false;
  const bool has_cars = !list(db, "SELECT name FROM main.sqlite_master WHERE type='table' AND name='Data_Car'").empty();
  bool allow_attach = false;
  sqlite3_set_authorizer(db, authorizer, &allow_attach);
  for (const Patch& p : patches) {
    const std::string label = p.mod + ":" + p.file.filename().string();
    const std::set<int64_t> before = has_cars ? car_ids(db) : std::set<int64_t>{};
    std::string why;
    std::vector<uint8_t> bytes;
    bool ok = read_file(p.file, bytes);
    if (!ok) why = "cannot read the file";
    if (ok && p.merge_db) {
      allow_attach = true;
      ok = merge_rows(db, bytes, &why);
      allow_attach = false;
    } else if (ok) {
      ok = exec(db, "SAVEPOINT modpatch", &why);
      if (ok) {
        const std::string sql(bytes.begin(), bytes.end());
        ok = exec(db, sql, &why);
        std::string ignore;
        if (!ok) exec(db, "ROLLBACK TO modpatch", &ignore);
        exec(db, "RELEASE modpatch", &ignore);
      }
    }
    if (!ok) {
      report.failed.push_back(label + ": " + why);
      continue;
    }
    report.applied.push_back(label);
    if (has_cars)
      for (int64_t id : car_ids(db))
        if (!before.count(id)) {
          report.cars_added.push_back(id);
          report.added_by[id] = "mod:" + p.mod;
        }
  }
  sqlite3_set_authorizer(db, nullptr, nullptr);
  return true;
}

void set_active(std::vector<Patch> patches) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_active = std::move(patches);
  g_last = Report{};
}

std::vector<Patch> active() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_active;
}

Report last_report() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_last;
}

void set_last_report(const Report& r) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last = r;
}

std::vector<int64_t> extend_catalog(std::map<int64_t, std::string>& baked,
                                    const std::vector<std::pair<int64_t, std::string>>& rows) {
  std::map<std::string, std::string> by_media;  // lower -> as baked
  for (const auto& [id, media] : baked) by_media.emplace(lower_ascii(media), media);
  std::vector<int64_t> added;
  for (const auto& [id, media] : rows) {
    if (baked.count(id)) continue;
    auto it = by_media.find(lower_ascii(media));
    if (it == by_media.end()) continue;
    baked[id] = it->second;
    added.push_back(id);
  }
  return added;
}

}  // namespace cars::db_patch
