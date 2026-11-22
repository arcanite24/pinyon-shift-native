#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "cars/sources.h"

namespace race::detail {

struct RaceDb {
  const cars::PartsDb* shared = nullptr;
  std::unique_ptr<cars::PartsDb> owned;
  sqlite3* raw = nullptr;

  ~RaceDb() {
    if (raw) sqlite3_close(raw);
  }

  bool open(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
            const cars::PartsDb* shared_db, std::string* error) {
    if (shared_db && shared_db->is_open()) {
      shared = shared_db;
      return true;
    }
    if (!state_root.empty() && !game_root.empty()) {
      cars::Install install;
      std::string why;
      if (cars::discover_install(game_root, state_root, "", install, &why)) {
        auto db = std::make_unique<cars::PartsDb>();
        if (db->open(install, &why)) {
          owned = std::move(db);
          shared = owned.get();
          return true;
        }
      }
    }
    const auto p = (game_root / "media" / "db" / "gamedb.slt").string();
    if (sqlite3_open_v2(p.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      if (error) *error = "cannot open " + p;
      if (raw) {
        sqlite3_close(raw);
        raw = nullptr;
      }
      return false;
    }
    return true;
  }

  bool each(const std::string& sql, const std::vector<std::string>& params,
            const std::function<void(sqlite3_stmt*)>& row, std::string* error) const {
    if (shared) return shared->each(sql, params, row, error);
    if (!raw) {
      if (error) *error = "database not open";
      return false;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(raw, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      if (error) *error = std::string("gamedb: ") + sqlite3_errmsg(raw);
      return false;
    }
    for (size_t i = 0; i < params.size(); ++i)
      sqlite3_bind_text(st, int(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) row(st);
    const bool ok = rc == SQLITE_DONE;
    if (!ok && error) *error = std::string("gamedb: ") + sqlite3_errmsg(raw);
    sqlite3_finalize(st);
    return ok;
  }
};

}  // namespace race::detail
