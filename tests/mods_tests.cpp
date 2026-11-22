// Slice 13A modding platform tests: manifests and load order, dependency
// and conflict resolution, modded-save isolation, SQL / merge-database
// patches (a synthetic car added), XML / INI / string-table patches,
// archive member overrides through fh1::Archive, hash-based replacement
// textures with live-reload cache invalidation, shader overrides, the
// native C-ABI plugin lifecycle and UDLC detection. With SHIFT_GAME_ROOT
// (or the sibling horizon1-recomp checkout) also a mod car added to the
// player's merged database.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <sqlite3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

#include "cars/database.h"
#include "cars/db_patch.h"
#include "cars/mod_manager.h"
#include "cars/mod_merge.h"
#include "cars/mod_plugins.h"
#include "cars/mods.h"
#include "cars/shader_mods.h"
#include "cars/sources.h"
#include "cars/texture_mods.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"

namespace fs = std::filesystem;

namespace {

int g_failed = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                           \
    }                                                                       \
  } while (0)

fs::path g_root;

fs::path fresh(const std::string& name) {
  const fs::path p = g_root / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

void write(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}

std::string read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

const cars::modmgr::Entry* entry(const cars::modmgr::Resolution& r, const std::vector<cars::modmgr::ModInfo>& mods,
                                 const std::string& name) {
  for (const auto& e : r.entries)
    if (mods[e.mod].name == name) return &e;
  return nullptr;
}

size_t order_of(const cars::modmgr::Resolution& r, const std::vector<cars::modmgr::ModInfo>& mods,
                const std::string& name) {
  for (size_t i = 0; i < r.entries.size(); ++i)
    if (mods[r.entries[i].mod].name == name) return i;
  return SIZE_MAX;
}

// ---------------------------------------------------------------- manifests

void test_manifests() {
  cars::modmgr::Manifest m;
  std::vector<std::string> warn;
  const char* toml =
      "# a mod\n"
      "name = \"hello\"\n"
      "title = \"Hello Mod\"\n"
      "author = 'Someone'\n"
      "version = \"1.2.0\"\n"
      "description = \"Line one\\nline two \\u00e9\"\n"
      "requires = [\"base_fix\", \"other\"]\n"
      "conflicts = [\n  \"bad\",\n  \"worse\"\n]\n"
      "priority = 5\n"
      "touches_save = false\n"
      "library = \"code/hello.dll\"\n"
      "game_version = \"v4\"\n"
      "[options.strength]\n"
      "label = \"Strength\"\n"
      "choices = [\"low\", \"high\"]\n"
      "default = 1\n";
  CHECK(cars::modmgr::parse_manifest_toml(toml, m, &warn));
  CHECK(warn.empty());
  CHECK(m.name == "hello" && m.title == "Hello Mod" && m.author == "Someone" && m.version == "1.2.0");
  CHECK(m.description == "Line one\nline two \xC3\xA9");
  CHECK(m.dependencies.size() == 2 && m.dependencies[1] == "other");
  CHECK(m.conflicts.size() == 2 && m.conflicts[1] == "worse");
  CHECK(m.priority == 5 && !m.touches_save && m.library == "code/hello.dll" && m.game_version == "v4");
  CHECK(m.options.size() == 1 && m.options[0].key == "strength" && m.options[0].choices.size() == 2 &&
        m.options[0].def == 1);

  cars::modmgr::Manifest t;
  cars::modmgr::parse_manifest_txt("title Old Style\nenabled 0\npriority 3\nrequires a,b\n", t);
  CHECK(t.title == "Old Style" && !t.enabled && t.priority == 3 && t.dependencies.size() == 2);

  cars::modmgr::Manifest bad;
  std::vector<std::string> w2;
  CHECK(!cars::modmgr::parse_manifest_toml("name = \nthis is not toml\n", bad, &w2));
  CHECK(w2.size() == 2);
}

// ---------------------------------------------------------------- load order

fs::path make_mod(const fs::path& root, const std::string& name, const std::string& toml) {
  write(root / name / "mod.toml", "name = \"" + name + "\"\n" + toml);
  return root / name;
}

void test_resolution() {
  const fs::path root = fresh("resolve");
  make_mod(root, "base_fix", "priority = 1\n");
  make_mod(root, "patch_for_fix", "requires = [\"base_fix\"]\n");  // must go above base_fix
  make_mod(root, "rival", "conflicts = [\"base_fix\"]\n");
  make_mod(root, "v4_only", "game_version = \"v4\"\n");
  make_mod(root, "needs_missing", "requires = [\"ghost\"]\n");
  make_mod(root, "chain", "requires = [\"needs_missing\"]\n");
  make_mod(root, "looks", "touches_save = false\n");
  make_mod(root, "off_by_default", "enabled = false\n");
  write(root / "_skipped" / "mod.toml", "name = \"_skipped\"\n");
  write(root / "no_manifest" / "readme.txt", "x");
  // An XE install record (tools/pinyon.py xe install's layout).
  write(root / "xe" / "xe-install.json", "{\"schema_version\": 1, \"version\": \"1.0\"}\n");
  write(root / "xe" / "game" / "media" / "db" / "gamedb.slt", "not really");

  auto mods = cars::modmgr::scan(root);
  CHECK(mods.size() == 9);  // _skipped and no_manifest are not mods

  cars::modmgr::Config cfg;
  cfg.order = {"rival", "base_fix", "patch_for_fix"};
  cfg.enabled["xe"] = false;
  auto r = cars::modmgr::resolve(mods, cfg, cars::modmgr::GameBuild::kBase);
  CHECK(r.entries.size() == 9);
  // patch_for_fix moved above base_fix; rival (listed first) wins the conflict.
  CHECK(order_of(r, mods, "patch_for_fix") < order_of(r, mods, "base_fix"));
  CHECK(order_of(r, mods, "rival") == 0);
  const auto* bf = entry(r, mods, "base_fix");
  CHECK(bf && bf->enabled && !bf->active && bf->reason == "conflicts with rival");
  const auto* pf = entry(r, mods, "patch_for_fix");
  CHECK(pf && !pf->active && pf->reason == "needs base_fix");  // cascades
  const auto* v4 = entry(r, mods, "v4_only");
  CHECK(v4 && !v4->active && v4->reason == "made for the v4 build");
  CHECK(!entry(r, mods, "needs_missing")->active && entry(r, mods, "needs_missing")->reason == "needs ghost");
  CHECK(!entry(r, mods, "chain")->active);
  CHECK(!entry(r, mods, "off_by_default")->enabled);
  CHECK(entry(r, mods, "looks")->active);
  CHECK(r.touches_save && r.save_profile == "modded");  // rival touches the save

  // Without rival the dependency chain loads, winner first.
  cfg.enabled["rival"] = false;
  r = cars::modmgr::resolve(mods, cfg, cars::modmgr::GameBuild::kV4);
  CHECK(entry(r, mods, "base_fix")->active && entry(r, mods, "patch_for_fix")->active && entry(r, mods, "v4_only")->active);
  std::vector<std::string> active_names;
  for (size_t i : r.active) active_names.push_back(mods[i].name);
  CHECK(!active_names.empty() && active_names[0] == "patch_for_fix");

  // Only looks-only mods: the player's own save.
  cars::modmgr::Config looks_only;
  for (const auto& m : mods) looks_only.enabled[m.name] = m.name == "looks";
  r = cars::modmgr::resolve(mods, looks_only, cars::modmgr::GameBuild::kBase);
  CHECK(r.active.size() == 1 && !r.touches_save && r.save_profile.empty());

  // XE: base only, its own save, hides Rally (and UDLC without the hotfix).
  cars::modmgr::Config xe_on;
  for (const auto& m : mods) xe_on.enabled[m.name] = m.name == "xe";
  r = cars::modmgr::resolve(mods, xe_on, cars::modmgr::GameBuild::kBase);
  CHECK(r.active.size() == 1 && r.save_profile == "xe");
  CHECK(std::find(r.hidden_dlc.begin(), r.hidden_dlc.end(), cars::modmgr::kRallyPackage) != r.hidden_dlc.end());
  CHECK(std::find(r.hidden_dlc.begin(), r.hidden_dlc.end(), cars::modmgr::kUdlcPackage) != r.hidden_dlc.end());
  CHECK(!r.notes.empty());
  r = cars::modmgr::resolve(mods, xe_on, cars::modmgr::GameBuild::kV4);
  CHECK(r.active.empty() && entry(r, mods, "xe")->reason == "made for the base build");
  const auto& xe = mods[entry(r, mods, "xe")->mod];
  CHECK(xe.manifest.kind == cars::modmgr::ModKind::kXe && xe.manifest.xe_version == "1.0" && xe.content.media == 1);

  // The config round-trips.
  cfg.options["looks"]["strength"] = "high";
  cfg.native_plugins = true;
  std::string err;
  CHECK(cars::modmgr::write_config(cars::modmgr::config_path(root), cfg, &err));
  cars::modmgr::Config back;
  CHECK(cars::modmgr::read_config(cars::modmgr::config_path(root), back));
  CHECK(back.order == cfg.order && back.enabled == cfg.enabled && back.options == cfg.options && back.native_plugins);
  // mods.toml is a file, not a mod.
  CHECK(cars::modmgr::scan(root).size() == 9);
}

// ---------------------------------------------------------------- save isolation

void test_isolation() {
  const fs::path root = fresh("isolation");
  const fs::path main = root / "drive";
  write(main / "save.txt", "shift-drive-save 1\ncar A\nposition 1 2 3\nheading 0\ncredits 500 500\n");
  write(main / "garage.txt", "garage 1\n");
  const fs::path mods_root = root / "mods";
  make_mod(mods_root, "cheat", "touches_save = true\n");
  make_mod(mods_root, "skin", "touches_save = false\n");
  make_mod(mods_root, "newgame", "profile = \"fresh\"\n");
  auto mods = cars::modmgr::scan(mods_root);
  cars::modmgr::Config cfg;
  cfg.enabled = {{"cheat", true}, {"skin", true}, {"newgame", false}};
  auto r = cars::modmgr::resolve(mods, cfg, cars::modmgr::GameBuild::kAny);
  std::string err;
  auto iso = cars::modmgr::isolate_save(main, mods, r, &err);
  CHECK(err.empty());
  CHECK(iso.isolated && iso.created && iso.save_dir == main / "user-modded");
  CHECK(read(iso.save_dir / "save.txt") == read(main / "save.txt"));
  CHECK(fs::exists(iso.save_dir / "garage.txt"));
  const std::string lock = read(iso.save_dir / "mods.lock");
  CHECK(lock.find("mod cheat") != std::string::npos && lock.find("mod skin") != std::string::npos);
  // Progress in the modded save never reaches the player's.
  write(iso.save_dir / "save.txt", "modded progress");
  iso = cars::modmgr::isolate_save(main, mods, r, &err);
  CHECK(!iso.created && read(iso.save_dir / "save.txt") == "modded progress");
  CHECK(read(main / "save.txt").find("credits 500") != std::string::npos);
  // Looks only: the player's save.
  cfg.enabled["cheat"] = false;
  r = cars::modmgr::resolve(mods, cfg, cars::modmgr::GameBuild::kAny);
  iso = cars::modmgr::isolate_save(main, mods, r, &err);
  CHECK(!iso.isolated && iso.save_dir == main);
  // A mod with its own profile starts a new save.
  cfg.enabled["newgame"] = true;
  r = cars::modmgr::resolve(mods, cfg, cars::modmgr::GameBuild::kAny);
  iso = cars::modmgr::isolate_save(main, mods, r, &err);
  CHECK(iso.isolated && iso.save_dir == main / "user-fresh" && !fs::exists(iso.save_dir / "save.txt"));
}

// ---------------------------------------------------------------- database patches

sqlite3* make_db() {
  sqlite3* db = nullptr;
  sqlite3_open(":memory:", &db);
  sqlite3_exec(db,
               "CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY, MediaName TEXT, DisplayName TEXT, PI INTEGER);"
               "INSERT INTO Data_Car VALUES (100, 'NIS_SkylineGTRVSpecII_02', 'Skyline', 461);"
               "INSERT INTO Data_Car VALUES (101, 'VW_Corrado_95', 'Corrado', 300);"
               "CREATE TABLE List_Events (Id INTEGER PRIMARY KEY, Name TEXT);",
               nullptr, nullptr, nullptr);
  return db;
}

int64_t scalar(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  int64_t v = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

void test_db_patches() {
  const fs::path root = fresh("dbpatch");
  // Low-priority mod: adds a clone of the Skyline and an event, sets PI.
  write(root / "carpack" / "db" / "01_cars.sql",
        "INSERT INTO Data_Car VALUES (9001, 'NIS_SkylineGTRVSpecII_02', 'Skyline Mod Edition', 500);\n"
        "INSERT INTO List_Events VALUES (77, 'Mod Cup');\n"
        "UPDATE Data_Car SET PI = 400 WHERE Id = 101;\n");
  // A broken script: rolled back alone (its first statement must not stick).
  write(root / "carpack" / "db" / "02_broken.sql",
        "INSERT INTO List_Events VALUES (78, 'Half');\nINSERT INTO Nope VALUES (1);\n");
  // A script that tries to reach a file is refused.
  write(root / "carpack" / "db" / "03_attach.sql", "ATTACH DATABASE 'evil.db' AS evil;\n");
  // The winner: balance over carpack's.
  write(root / "balance" / "db" / "balance.sql", "UPDATE Data_Car SET PI = 450 WHERE Id = 101;\n");
  // A merge database (whole rows by key), as packages ship.
  {
    sqlite3* m = nullptr;
    sqlite3_open(":memory:", &m);
    sqlite3_exec(m,
                 "CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY, MediaName TEXT, DisplayName TEXT, PI INTEGER, Extra INTEGER);"
                 "INSERT INTO Data_Car VALUES (9002, 'VW_Corrado_95', 'Corrado Merge', 320, 1);",
                 nullptr, nullptr, nullptr);
    sqlite3_int64 size = 0;
    unsigned char* bytes = sqlite3_serialize(m, "main", &size, 0);
    fs::create_directories(root / "mergepack" / "db");
    std::ofstream g(root / "mergepack" / "db" / "cars_merge.slt", std::ios::binary);
    g.write(reinterpret_cast<const char*>(bytes), std::streamsize(size));
    g.close();
    sqlite3_free(bytes);
    sqlite3_close(m);
  }
  const auto patches = cars::db_patch::collect(
      {{"balance", root / "balance"}, {"mergepack", root / "mergepack"}, {"carpack", root / "carpack"}});
  CHECK(patches.size() == 5);
  CHECK(patches.front().mod == "carpack" && patches.back().mod == "balance");  // the winner applies last
  sqlite3* db = make_db();
  cars::db_patch::Report rep;
  CHECK(cars::db_patch::apply(db, patches, rep));
  CHECK(rep.applied.size() == 3);
  CHECK(rep.failed.size() == 2);
  CHECK(scalar(db, "SELECT COUNT(*) FROM Data_Car") == 4);
  CHECK(scalar(db, "SELECT PI FROM Data_Car WHERE Id = 9001") == 500);
  CHECK(scalar(db, "SELECT PI FROM Data_Car WHERE Id = 101") == 450);  // balance won
  CHECK(scalar(db, "SELECT COUNT(*) FROM List_Events WHERE Id = 77") == 1);
  CHECK(scalar(db, "SELECT COUNT(*) FROM List_Events WHERE Id = 78") == 0);  // rolled back
  CHECK(scalar(db, "SELECT COUNT(*) FROM Data_Car WHERE Id = 9002") == 1);
  CHECK(rep.cars_added.size() == 2 && rep.added_by.at(9001) == "mod:carpack" && rep.added_by.at(9002) == "mod:mergepack");
  sqlite3_close(db);

  // The catalogue picks up cars that reuse a baked model.
  std::map<int64_t, std::string> baked{{100, "nis_skylinegtrvspecii_02"}};
  const auto added = cars::db_patch::extend_catalog(baked, {{9001, "NIS_SkylineGTRVSpecII_02"}, {9002, "VW_Corrado_95"}});
  CHECK(added.size() == 1 && baked.at(9001) == "nis_skylinegtrvspecii_02" && !baked.count(9002));
}

fs::path find_game_root() {
  if (const char* g = std::getenv("SHIFT_GAME_ROOT")) return g;
  for (const char* p : {R"(C:\Users\neri\code\horizon1-recomp\.local\game\base)", "../horizon1-recomp/.local/game/base"})
    if (fs::exists(fs::path(p) / "media" / "db" / "gamedb.slt")) return p;
  return {};
}

void test_disc_database_mod_car() {
  const fs::path game = find_game_root();
  if (game.empty()) {
    std::printf("  (skipping the disc database mod car: no game root)\n");
    return;
  }
  const fs::path root = fresh("disc_db");
  write(root / "clone" / "db" / "clone.sql",
        "CREATE TEMP TABLE t AS SELECT * FROM Data_Car WHERE MediaName = 'NIS_SkylineGTRVSpecII_02';\n"
        "UPDATE t SET Id = 999001;\n"
        "INSERT INTO Data_Car SELECT * FROM t;\n"
        "DROP TABLE t;\n");
  cars::db_patch::set_active(cars::db_patch::collect({{"clone", root / "clone"}}));
  cars::Install install;
  std::string err;
  CHECK(cars::discover_install(game, {}, {}, install, &err));
  cars::Database db;
  CHECK(cars::load_database(install, db, &err));
  bool found = false;
  for (const auto& c : db.cars)
    if (c.id == 999001) found = c.origin == "mod:clone" && c.media_name == "NIS_SkylineGTRVSpecII_02";
  CHECK(found);
  CHECK(cars::db_patch::last_report().cars_added.size() == 1);
  std::printf("  disc database: %zu cars with the mod car\n", db.cars.size());
  cars::db_patch::set_active({});
}

// ---------------------------------------------------------------- XML / INI / strings

void test_xml_ini_strings() {
  std::string base =
      "<?xml version=\"1.0\"?>\n"
      "<!-- traffic -->\n"
      "<AIOpenWorld>\n"
      "  <Settings name=\"freeroam\">\n"
      "    <CarList numInitialTrafficCars=\"40\" numInitialFestivalCars=\"8\"/>\n"
      "  </Settings>\n"
      "  <Settings name=\"race\"><CarList numInitialTrafficCars=\"10\"/></Settings>\n"
      "  <Layer gain=\"1\"/><Layer gain=\"2\"/>\n"
      "  <Title>Old</Title>\n"
      "</AIOpenWorld>\n";
  const char* patch =
      "<AIOpenWorld>\n"
      "  <Settings name=\"freeroam\"><CarList numInitialTrafficCars=\"0\" numInitialFestivalCars=\"0\"/></Settings>\n"
      "  <Layer/><Layer gain=\"9\"/>\n"
      "  <Layer gain=\"3\" shift-add=\"true\"/>\n"
      "  <Title>New</Title>\n"
      "  <Settings name=\"night\"><CarList numInitialTrafficCars=\"5\"/></Settings>\n"
      "</AIOpenWorld>\n";
  std::string err;
  CHECK(cars::mod_merge::merge_xml(base, patch, &err));
  CHECK(base.find("<?xml version=\"1.0\"?>") == 0 && base.find("<!-- traffic -->") != std::string::npos);
  CHECK(base.find("<Settings name=\"freeroam\">") != std::string::npos);
  CHECK(base.find("numInitialTrafficCars=\"0\" numInitialFestivalCars=\"0\"") != std::string::npos);
  CHECK(base.find("<CarList numInitialTrafficCars=\"10\"/>") != std::string::npos);  // race untouched
  CHECK(base.find("<Layer gain=\"1\"/><Layer gain=\"9\"/>") != std::string::npos);    // by position
  CHECK(base.find("<Layer gain=\"3\"/>") != std::string::npos);                      // appended, marker gone
  CHECK(base.find("<Title>New</Title>") != std::string::npos && base.find("Old") == std::string::npos);
  CHECK(base.find("<Settings name=\"night\">") != std::string::npos);
  std::string root_mismatch = "<A/>";
  CHECK(!cars::mod_merge::merge_xml(root_mismatch, "<B/>", &err));

  std::string ini = "; graphics\r\nquality=1\r\n[Shadows]\r\nsize = 1024\r\ncascades=2\r\n\r\n[Sky]\r\nclouds=1\r\n";
  cars::mod_merge::merge_ini(ini, "quality=3\n[shadows]\nsize=4096\nfilter=pcf\n[Water]\nreflect=1\n");
  CHECK(ini.find("quality=3\r\n") != std::string::npos);
  CHECK(ini.find("size = 4096\r\n") != std::string::npos);
  CHECK(ini.find("cascades=2\r\nfilter=pcf\r\n") != std::string::npos);
  CHECK(ini.find("[Sky]\r\nclouds=1\r\n") != std::string::npos);
  CHECK(ini.find("[Water]\r\nreflect=1\r\n") != std::string::npos);

  // String tables: replace one IDS_ name, add another; _&N resolves.
  fh1::StringTables tables;
  fh1::StringTable t;
  t.keys = {fh1::string_hash("IDS_Old")};
  t.text = {u"Old text"};
  tables.put("CarNames.str", t);
  std::vector<std::pair<std::string, std::string>> entries;
  CHECK(cars::mod_merge::parse_string_patch("IDS_Old = \"Replaced\"\nIDS_ModCar = \"Skyline Mod Edition \\u00e9\"\n",
                                            entries, &err));
  CHECK(entries.size() == 2);
  CHECK(cars::mod_merge::apply_string_patch(tables.table("CarNames"), entries) == 2);
  auto ref = [](const char* table, const char* ids) {
    return "_&" + std::to_string((uint32_t(fh1::string_hash(table)) << 16) | fh1::string_hash(ids));
  };
  CHECK(tables.resolve(ref("CarNames", "IDS_Old")) == std::optional<std::string>("Replaced"));
  CHECK(tables.resolve(ref("CarNames", "IDS_ModCar")) == std::optional<std::string>("Skyline Mod Edition \xC3\xA9"));
  const auto& keys = tables.table("CarNames").keys;
  CHECK(std::is_sorted(keys.begin(), keys.end()));
}

// A stored ZIP with the given members.
void write_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& members) {
  std::string out, central;
  auto le16 = [](std::string& s, uint32_t v) { s += char(v & 0xFF), s += char((v >> 8) & 0xFF); };
  auto le32 = [&](std::string& s, uint32_t v) { le16(s, v & 0xFFFF), le16(s, v >> 16); };
  for (const auto& [name, data] : members) {
    const uint32_t crc = fh1::crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    const uint32_t offset = uint32_t(out.size());
    le32(out, 0x04034B50), le16(out, 20), le16(out, 0), le16(out, 0), le16(out, 0), le16(out, 0);
    le32(out, crc), le32(out, uint32_t(data.size())), le32(out, uint32_t(data.size()));
    le16(out, uint32_t(name.size())), le16(out, 0);
    out += name + data;
    le32(central, 0x02014B50), le16(central, 20), le16(central, 20), le16(central, 0), le16(central, 0);
    le16(central, 0), le16(central, 0), le32(central, crc), le32(central, uint32_t(data.size()));
    le32(central, uint32_t(data.size())), le16(central, uint32_t(name.size())), le16(central, 0), le16(central, 0);
    le16(central, 0), le16(central, 0), le32(central, 0), le32(central, offset);
    central += name;
  }
  const uint32_t cd_offset = uint32_t(out.size());
  out += central;
  le32(out, 0x06054B50), le16(out, 0), le16(out, 0), le16(out, uint32_t(members.size()));
  le16(out, uint32_t(members.size())), le32(out, uint32_t(central.size())), le32(out, cd_offset), le16(out, 0);
  write(p, out);
}

std::string read_member(const fs::path& zip, const std::string& name) {
  auto a = fh1::Archive::open(zip, nullptr);
  if (!a) return "(no archive)";
  auto r = a->read(name);
  return r.ok() ? std::string(r.data.begin(), r.data.end()) : "(error: " + r.error + ")";
}

void test_member_overrides() {
  const fs::path root = fresh("members");
  const fs::path zip = root / "game" / "media" / "Physics.zip";
  write_zip(zip, {{"tuning/a.xml", "<A v=\"1\"/>"},
                  {"tuning/b.xml", "<B><C x=\"1\"/></B>"},
                  {"cfg/render.ini", "[Main]\nlod=1\n"},
                  {"keep.txt", "untouched"}});
  CHECK(read_member(zip, "tuning/a.xml") == "<A v=\"1\"/>");
  const fs::path mods = root / "mods";
  write(mods / "winner" / "members" / "physics.zip" / "tuning" / "a.xml", "<A v=\"winner\"/>");
  write(mods / "loser" / "members" / "physics.zip" / "tuning" / "a.xml", "<A v=\"loser\"/>");
  write(mods / "loser" / "xml_patches" / "media" / "physics.zip" / "tuning" / "b.xml", "<B><C x=\"2\"/></B>");
  write(mods / "winner" / "merge" / "physics.zip" / "tuning" / "b.xml", "<B><C y=\"3\"/></B>");
  write(mods / "winner" / "merge" / "physics.zip" / "cfg" / "render.ini", "[Main]\nlod=3\n");
  write(mods / "winner" / "add" / "physics.zip" / "new" / "added.txt", "a new member");
  write(mods / "winner" / "members" / "other.zip" / "tuning" / "a.xml", "<A v=\"other\"/>");
  const auto counts = cars::mod_merge::set_active({mods / "winner", mods / "loser"});
  CHECK(counts.members == 3 && counts.xml == 3);
  CHECK(read_member(zip, "tuning/a.xml") == "<A v=\"winner\"/>");
  CHECK(read_member(zip, "TUNING\\A.XML") == "<A v=\"winner\"/>");
  CHECK(read_member(zip, "tuning/b.xml") == "<B><C x=\"2\" y=\"3\"/></B>");  // both merges, the winner last
  CHECK(read_member(zip, "cfg/render.ini") == "[Main]\nlod=3\n");
  CHECK(read_member(zip, "new/added.txt") == "a new member");
  CHECK(read_member(zip, "keep.txt") == "untouched");
  cars::mod_merge::set_active({});
  CHECK(read_member(zip, "tuning/a.xml") == "<A v=\"1\"/>");
  CHECK(read_member(zip, "new/added.txt").rfind("(error", 0) == 0);
}

// ---------------------------------------------------------------- textures

bool write_png(const fs::path& p, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for (size_t i = 0; i < px.size(); i += 4) px[i] = r, px[i + 1] = g, px[i + 2] = b, px[i + 3] = 255;
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  return stbi_write_png(p.string().c_str(), int(w), int(h), 4, px.data(), int(w * 4)) != 0;
}

void test_textures() {
  using namespace cars::texture_mods;
  CHECK(xxh64("", 0) == 0xEF46DB3751D8E999ull);
  CHECK(xxh64("a", 1) == 0xD24EC4F1A98C6E5Bull);
  const std::string longer(100, 'x');
  CHECK(xxh64(longer.data(), longer.size()) == xxh64(longer.data(), longer.size()));
  CHECK(xxh64(longer.data(), longer.size()) != xxh64(longer.data(), longer.size() - 1));
  CHECK(scale_factor(64, 32, 512, 256) == 8 && scale_factor(64, 32, 96, 48) == 0 && scale_factor(64, 32, 64, 32) == 1);

  cars::bake::Texture base;
  base.name = "nodamage_lod0.xds";
  base.format = cars::bake::TextureFormat::kRgba8;
  base.levels.push_back({4, 4, std::vector<uint8_t>(64, 0x40)});
  const uint64_t h = content_hash(base.levels[0].data);
  const fs::path mods = fresh("textures");
  const fs::path file = mods / "hd" / "textures" / (hex16(h) + ".png");
  CHECK(write_png(file, 16, 16, 255, 0, 0));  // 4x
  write_png(mods / "low" / "textures" / (hex16(h) + ".png"), 4, 4, 0, 0, 255);  // loses to hd
  CHECK(write_png(mods / "hd" / "textures" / "nodamage_lod1.png", 12, 12, 0, 255, 0));  // wrong scale (3x)
  registry().scan({mods / "hd", mods / "low"});
  CHECK(registry().size() == 2);
  cars::bake::Texture out;
  std::string why;
  CHECK(substitute(base, out, &why));
  CHECK(out.levels.size() == 5 && out.levels[0].width == 16 && out.levels.back().width == 1);
  CHECK(out.levels[0].data[0] == 255 && out.levels[0].data[1] == 0);
  // A name match with the wrong scale is refused.
  cars::bake::Texture other = base;
  other.name = "NODAMAGE_LOD1.xds";
  other.levels[0].data.assign(64, 0x11);
  CHECK(!substitute(other, out, &why) && why.find("not 1, 2, 4 or 8") != std::string::npos);
  CHECK(registry().refused().size() == 1);

  // Live reload: an edited file is reported, the cache drops it, and the
  // next upload reads the new pixels.
  const uint32_t gen = registry().generation();
  CHECK(registry().reload().empty());
  CHECK(write_png(file, 32, 32, 0, 0, 255));  // 8x, blue
  auto changed = registry().reload();
  CHECK(changed.size() == 1 && changed[0] == content_key(h) && registry().generation() == gen + 1);
  CHECK(substitute(base, out, &why) && out.levels[0].width == 32 && out.levels[0].data[2] == 255);
  // Removing the winner falls back to the next mod's file.
  fs::remove(file);
  changed = registry().reload();
  CHECK(changed.size() == 1);
  CHECK(substitute(base, out, &why) && out.levels[0].width == 4 && out.levels[0].data[2] == 255);
  fs::remove(mods / "low" / "textures" / (hex16(h) + ".png"));
  registry().reload();
  CHECK(!substitute(base, out, &why));

  // The cache by itself.
  TextureCache cache;
  const fs::path cpng = mods / "cache.png";
  write_png(cpng, 2, 2, 1, 2, 3);
  CHECK(cache.get("k", cpng, &why) && cache.get("k", cpng, &why) && cache.decodes() == 1);
  CHECK(cache.invalidate({"k", "missing"}) == 1 && cache.size() == 0);
  CHECK(cache.get("k", cpng, &why) && cache.decodes() == 2);

  // DDS: uncompressed BGRA and BC1.
  auto dds = [](uint32_t w, uint32_t h, bool bc1, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b(128, 0);
    std::memcpy(b.data(), "DDS ", 4);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); };
    put(4, 124), put(12, h), put(16, w), put(76, 32);
    if (bc1) {
      put(80, 0x4);
      std::memcpy(&b[84], "DXT1", 4);
    } else {
      put(80, 0x41), put(88, 32), put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF), put(104, 0xFF000000);
    }
    b.insert(b.end(), data.begin(), data.end());
    return b;
  };
  Image img;
  CHECK(decode_dds(dds(1, 1, false, {10, 20, 30, 40}), img, &why) && img.rgba == std::vector<uint8_t>({30, 20, 10, 40}));
  // BC1 block: colour0 pure red (0xF800), colour1 black, all indices 0.
  CHECK(decode_dds(dds(4, 4, true, {0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0}), img, &why));
  CHECK(img.width == 4 && img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[3] == 255);
  CHECK(!decode_dds(std::vector<uint8_t>(10, 0), img, &why));
  registry().clear();
}

// ---------------------------------------------------------------- shaders

void test_shaders() {
  const fs::path mods = fresh("shaders");
  std::vector<uint32_t> words = {0x07230203u, 0x00010000u, 0, 1, 0};
  std::string spv(reinterpret_cast<const char*>(words.data()), words.size() * 4);
  write(mods / "a" / "shaders" / "world.frag.spv", spv);
  write(mods / "a" / "shaders" / "ui.vert.spv", "not spirv at all");
  words[3] = 7;
  write(mods / "b" / "shaders" / "world.frag.spv", std::string(reinterpret_cast<const char*>(words.data()), words.size() * 4));
  write(mods / "b" / "shaders" / "post.frag.spv", spv);
  auto& reg = cars::shader_mods::registry();
  reg.scan({mods / "a", mods / "b"});
  CHECK(reg.size() == 2);
  auto w = reg.find("WORLD.FRAG");
  CHECK(w && w->size() == 5 && (*w)[3] == 1);  // a's (the winner)
  CHECK(reg.find("post.frag").has_value() && !reg.find("ui.vert").has_value());
  CHECK(reg.refused().count("ui.vert") == 1);
  CHECK(cars::shader_mods::valid_spirv(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(spv.data()), spv.size())));
  reg.clear();
}

// ---------------------------------------------------------------- native plugins

void test_plugins() {
#ifdef SHIFT_TEST_PLUGIN
  cars::plugins::Host host;
  std::vector<std::string> log;
  std::vector<std::string> notices;
  host.set_log([&](const std::string& mod, std::string_view m) { log.push_back(mod + ": " + std::string(m)); });
  host.set_notify([&](const std::string&, std::string_view m, float) { notices.emplace_back(m); });
  std::string err;
  CHECK(host.load("sample", g_root, SHIFT_TEST_PLUGIN, {{"greeting", "hi"}}, &err));
  CHECK(host.alive() == 1);
  CHECK(!notices.empty() && notices[0] == "sample mod loaded");
  ShiftModFrame f{};
  f.size = sizeof f;
  f.speed = 12.5f;
  f.gear = 3;
  for (uint32_t i = 0; i < 5; ++i) {
    f.frame = i;
    host.tick(f);
  }
  CHECK(host.plugins()[0].ticks == 5);
  host.shutdown();
  CHECK(host.alive() == 0);
  auto has = [&](const std::string& s) {
    for (const auto& l : log)
      if (l == s) return true;
    return false;
  };
  CHECK(has("sample: init abi=1 greeting=hi"));
  CHECK(has("sample: tick 3 speed 12.5 gear 3"));
  CHECK(has("sample: shutdown after 5 ticks"));
  // A missing library is refused and listed, never alive.
  CHECK(!host.load("ghost", g_root, g_root / ("ghost" + std::string(cars::plugins::library_suffix())), {}, &err));
  CHECK(host.plugins().back().mod == "ghost" && !host.plugins().back().alive && !host.plugins().back().error.empty());
#else
  std::printf("  (skipping plugins: no sample plugin built)\n");
#endif
}

// ---------------------------------------------------------------- UDLC

void test_udlc() {
  const fs::path state = fresh("udlc_state");
  auto st = cars::modmgr::detect_udlc(state);
  CHECK(!st.installed && !st.mountable() && !st.problem.empty());
  const fs::path title = state / "user" / "0000000000000000" / "4D5309C9";
  fs::create_directories(title / "00000002" / cars::modmgr::kUdlcPackage / "Media" / "DLCZips");
  write(title / "Headers" / "00000002" / (std::string(cars::modmgr::kUdlcPackage) + ".header"), "h");
  write(state / "dlc" / (std::string(cars::modmgr::kUdlcPackage) + ".json"),
        "{\"package_id\": \"4D5309C900000729\", \"display_name\": \"UDLC Pack V1.2\", \"version\": \"1.2\"}");
  st = cars::modmgr::detect_udlc(state);
  CHECK(st.installed && st.header && st.enabled && st.version == "1.2" && st.mountable());
  CHECK(!st.traffic_mountable());
  fs::create_directories(title / "Disabled" / "00000002" / cars::modmgr::kUdlcTrafficPackage);
  write(title / "Headers" / "00000002" / (std::string(cars::modmgr::kUdlcTrafficPackage) + ".header"), "h");
  st = cars::modmgr::detect_udlc(state);
  CHECK(st.traffic_installed && !st.traffic_enabled && !st.traffic_mountable());
  write(state / "dlc" / (std::string(cars::modmgr::kUdlcPackage) + ".json"), "{\"version\": \"1.11\"}");
  st = cars::modmgr::detect_udlc(state);
  CHECK(!st.mountable() && st.problem.find("1.11") != std::string::npos);

  // What discover_install sees.
  cars::modmgr::set_udlc_mount(false, false);
  CHECK(cars::modmgr::package_hidden(cars::modmgr::kUdlcPackage));
  CHECK(cars::modmgr::package_hidden(cars::modmgr::kUdlcTrafficPackage));
  cars::modmgr::set_udlc_mount(true, false);
  CHECK(!cars::modmgr::package_hidden(cars::modmgr::kUdlcPackage));
  CHECK(cars::modmgr::package_hidden(cars::modmgr::kUdlcTrafficPackage));  // the unlocker needs both
  cars::modmgr::set_udlc_mount(true, true);
  CHECK(!cars::modmgr::package_hidden(cars::modmgr::kUdlcTrafficPackage));
  cars::modmgr::set_hidden_packages({"6f6992766050d818245add408031e280fb5f4e634d"});
  CHECK(cars::modmgr::package_hidden(cars::modmgr::kRallyPackage));
  cars::modmgr::set_hidden_packages({});
  cars::modmgr::set_udlc_mount(false, false);
  CHECK(!cars::modmgr::package_hidden(cars::modmgr::kRallyPackage));
}

// ---------------------------------------------------------------- activation

void test_activation() {
  const fs::path root = fresh("activate");
  const fs::path bake = root / "play" / "bake";
  write(bake / "cars" / "a.physics.txt", "base");
  make_mod(root / "mods", "pack", "");
  write(root / "mods" / "pack" / "bake" / "cars" / "a.physics.txt", "modded");
  write(root / "mods" / "pack" / "db" / "x.sql", "SELECT 1;");
  write(root / "mods" / "pack" / "textures" / "0123456789abcdef.png", "x");
  write(root / "mods" / "pack" / "stringtables" / "EN" / "CarNames.toml", "IDS_A = \"a\"\n");
  auto mods = cars::modmgr::scan(root / "mods");
  CHECK(mods.size() == 1 && mods[0].content.bake == 1 && mods[0].content.db == 1 && mods[0].content.textures == 1);
  CHECK(mods[0].content.summary() == "CARS 1  DB 1  TEXT 1  TEX 1");
  auto r = cars::modmgr::resolve(mods, {}, cars::modmgr::GameBuild::kAny);
  cars::modmgr::ActivateOptions opts;
  opts.bake_dir = bake;
  const auto rep = cars::modmgr::activate(mods, r, opts);
  CHECK(rep.db_patches == 1 && rep.textures == 1 && rep.string_patches == 1);
  CHECK(cars::mods::resolve(bake / "cars" / "a.physics.txt") == root / "mods" / "pack" / "bake" / "cars" / "a.physics.txt");
  CHECK(cars::db_patch::active().size() == 1);
  // Deactivating clears everything.
  cars::modmgr::activate(mods, cars::modmgr::Resolution{}, opts);
  CHECK(cars::db_patch::active().empty() && cars::texture_mods::registry().size() == 0);
  CHECK(cars::mods::resolve(bake / "cars" / "a.physics.txt") == bake / "cars" / "a.physics.txt");
}

}  // namespace

int main() {
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / "shift_mods_tests";
  fs::remove_all(g_root, ec);
  fs::create_directories(g_root, ec);
  test_manifests();
  test_resolution();
  test_isolation();
  test_db_patches();
  test_disc_database_mod_car();
  test_xml_ini_strings();
  test_member_overrides();
  test_textures();
  test_shaders();
  test_plugins();
  test_udlc();
  test_activation();
  fs::remove_all(g_root, ec);
  if (g_failed) {
    std::fprintf(stderr, "mods_tests: %d failed\n", g_failed);
    return 1;
  }
  std::printf("mods_tests: all passed\n");
  return 0;
}
