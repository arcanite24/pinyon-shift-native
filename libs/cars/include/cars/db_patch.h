// Game database patch mods (ours, Slice 13): SQL scripts and merge
// databases from enabled mods, applied to the merged in-memory database
// (database.cpp: v4's, or the disc's, with the packages' merges) after the
// packages, so mods can add cars, events and parts or change balance.
//
// A mod's db/ folder holds *.sql scripts and *.slt merge databases (whole
// rows by key, like a package's merge). Mods apply in reverse load order
// (the winner last, so its rows win); within a mod, by file name. Each file
// runs inside a savepoint: a script that fails is rolled back alone and
// reported, the others still apply. Scripts cannot ATTACH or DETACH other
// databases (an authorizer denies it), so a patch only touches the
// in-memory copy; the player's files are never written.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace cars::db_patch {

struct Patch {
  std::string mod;
  std::filesystem::path file;
  bool merge_db = false;  // a .slt merge database, else an SQL script
};

// The patches of `mods` (name and folder, the winner first) in apply order.
std::vector<Patch> collect(const std::vector<std::pair<std::string, std::filesystem::path>>& mods_winner_first);

struct Report {
  std::vector<std::string> applied;  // "mod:file"
  std::vector<std::string> failed;   // "mod:file: why"
  std::vector<int64_t> cars_added;   // Data_Car ids the patches added
  std::map<int64_t, std::string> added_by;  // car id -> "mod:<name>"
};

// Applies `patches` to `db` (its main schema). False only when the
// database itself cannot be used; failed patches are in the report.
bool apply(sqlite3* db, const std::vector<Patch>& patches, Report& report);

// The patches database.cpp applies on every merged open (set by
// modmgr::activate), and the last open's report.
void set_active(std::vector<Patch> patches);
std::vector<Patch> active();
Report last_report();
void set_last_report(const Report& r);

// Adds to a Data_Car id -> media map every row (id, media) whose media the
// map already has (case-insensitive): a mod's car that reuses a baked
// model joins the catalogue. Returns the ids added.
std::vector<int64_t> extend_catalog(std::map<int64_t, std::string>& baked,
                                    const std::vector<std::pair<int64_t, std::string>>& rows);

}  // namespace cars::db_patch
