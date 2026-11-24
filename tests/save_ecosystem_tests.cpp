// Slice 13A Save Ecosystem tests:
//  1. Rolling automatic save backups (session start, 10-min autosave throttle,
//     clean exit, achievements + replays metadata, 10-backup rotation,
//     restore with before-restore snapshot and SHA-256 tamper rejection).
//  2. Multi-profile & Gamertag Manager (validation, create, gamertag edit,
//     profile switch, persistence, safe remove moving folder aside).
//  3. Cross-Device Save Sync (SAVE_SYNC.md): pack/unpack, initial auto-pull,
//     continuation fast-forward with before-sync backup, divergence conflict
//     detection, choose_load and choose_keep resolution, v4 one-way guard,
//     damaged archive and sync-conflict copy ignoring, unreachable folder.
//  4. Streamlined Pinyon Shift & Xenia Save Import: plaintext export folder,
//     nested user/ export, ZIP archive extraction, .pre-import + before-import
//     backups, v4 vs base guard, encrypted console ForzaProfile refusal (zero
//     console keys).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "drive/save.h"
#include "fh1data/archive.h"
#include "garage/garage.h"
#include "profile/backup.h"
#include "profile/profiles.h"
#include "profile/sync.h"
#include "profile/xenia_import.h"

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

void write_file(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(data.data(), std::streamsize(data.size()));
}

void write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

void write_sample_save(const fs::path& dir, const std::string& car, int64_t credits) {
  drive::DriveSave s;
  s.car = car;
  s.position = {100.0, 20.0, 300.0};
  s.heading = 1.2;
  s.credits = credits;
  s.total_winnings = credits;
  std::string err;
  CHECK(drive::write_save(dir / "save.txt", s, &err));
  write_file(dir / "garage.txt", "shift-garage 1\ncar " + car + " " + std::to_string(credits) + "\n");
}

// ---------------------------------------------------------------- 1. Backups

void test_rolling_backups() {
  const fs::path root = fresh("backups");
  profile::backup::Config cfg;
  cfg.save_dir = root / "drive";
  cfg.backups_dir = root / "backups" / "saves";
  cfg.achievements = root / "achievements.txt";
  cfg.replays_dir = root / "replays";
  cfg.keep = 10;

  // Empty save dir: snapshot skipped cleanly.
  bool skipped = false;
  std::string err;
  CHECK(profile::backup::snapshot(cfg, "start", nullptr, &err, 1700000000, &skipped) && skipped);

  write_sample_save(cfg.save_dir, "NIS_SkylineGTRVSpecII_02", 10000);
  write_file(cfg.achievements, "ach 1\n");
  write_file(cfg.replays_dir / "race01.rep", "replay-bytes-12345");

  // AutoBackup schedule: start -> throttled autosaves (< 600s ignored) -> 10m autosave -> exit.
  profile::backup::AutoBackup ab(cfg);
  CHECK(ab.on_session_start(0.0, 1700000100));
  CHECK(!ab.on_saved(120.0, 1700000220));  // < 10 min
  CHECK(!ab.on_saved(599.0, 1700000699));  // < 10 min
  write_sample_save(cfg.save_dir, "VW_Corrado_95", 25000);
  CHECK(ab.on_saved(600.0, 1700000700));   // >= 10 min: snapshots!
  CHECK(!ab.on_saved(900.0, 1700001000));  // throttled again
  CHECK(ab.on_exit(950.0, 1700001050));
  CHECK(ab.snapshots_taken() == 3);

  auto list = profile::backup::list(cfg);
  CHECK(list.size() == 3);
  CHECK(list[0].reason == "exit" && list[0].credits == 25000);
  CHECK(list[2].reason == "start" && list[2].credits == 10000 && list[2].car == "NIS_SkylineGTRVSpecII_02");
  // Replay metadata was captured without copying the replay binary.
  CHECK(fs::exists(list[0].dir / "replays.txt"));
  CHECK(read_file(list[0].dir / "replays.txt").find("race01.rep") != std::string::npos);
  CHECK(fs::exists(list[0].dir / "achievements.txt"));

  // Rolling 10-backup rotation: take 10 more -> 13 total -> rotated to newest 10.
  for (int i = 0; i < 10; ++i) {
    write_sample_save(cfg.save_dir, "NIS_SkylineGTRVSpecII_02", 30000 + i * 1000);
    CHECK(profile::backup::snapshot(cfg, "autosave", nullptr, &err, 1700002000 + i * 60));
  }
  list = profile::backup::list(cfg);
  CHECK(list.size() == 10);
  CHECK(list.front().credits == 39000);
  CHECK(list.back().credits == 30000);

  // Restore the oldest kept backup (30000 cr): creates before-restore of 39000 cr, restores 30000 cr.
  const std::string target_id = list.back().id;
  write_file(cfg.achievements, "ach 99\n");
  CHECK(profile::backup::restore(cfg, target_id, &err, 1700009000));
  drive::DriveSave restored;
  CHECK(drive::read_save(cfg.save_dir / "save.txt", restored, &err));
  CHECK(restored.credits == 30000);
  CHECK(read_file(cfg.achievements) == "ach 1\n");
  list = profile::backup::list(cfg);
  CHECK(list.size() == 10);
  CHECK(list.front().reason == "before-restore" && list.front().credits == 39000);

  // Tampered backup is rejected via SHA-256 check.
  const std::string tamper_id = list[1].id;
  write_file(list[1].dir / "save.txt", "corrupted");
  CHECK(!profile::backup::restore(cfg, tamper_id, &err, 1700009100));
  CHECK(err.find("damaged") != std::string::npos);
}

// ---------------------------------------------------------------- 2. Multi-Profile & Gamertag

void test_profiles() {
  const fs::path root = fresh("profiles");
  std::string why;
  CHECK(profile::profiles::valid_gamertag("Horizon Driver", &why));
  CHECK(profile::profiles::valid_gamertag("Neri99", &why));
  CHECK(!profile::profiles::valid_gamertag("", &why));
  CHECK(!profile::profiles::valid_gamertag("123BadStart", &why));
  CHECK(!profile::profiles::valid_gamertag("Too  ManySpaces", &why));
  CHECK(!profile::profiles::valid_gamertag("WayTooLongGamertag16", &why));
  CHECK(!profile::profiles::valid_gamertag("Bad$Tag", &why));
  CHECK(profile::profiles::name_from_gamertag("Weekend Racer") == "weekend-racer");

  profile::profiles::Manager mgr(root, root / "drive");
  CHECK(mgr.load(&why));
  CHECK(mgr.profiles().size() == 1 && mgr.active().name == "default" && mgr.active().gamertag == "Player");

  CHECK(mgr.set_gamertag("default", "Colorado King", &why));
  CHECK(mgr.create("weekend", "Weekend Racer", &why));
  CHECK(mgr.create("rally-run", "Dirt Ace", &why));
  CHECK(!mgr.create("weekend", "Dup", &why));  // duplicate refused
  CHECK(mgr.switch_to("weekend", &why));
  CHECK(mgr.active().name == "weekend" && mgr.active().gamertag == "Weekend Racer");
  CHECK(mgr.active().save_dir == root / "profiles" / "weekend");

  // Reload from profiles.txt preserves all profiles, gamertags, and active slot.
  profile::profiles::Manager mgr2(root, root / "drive");
  CHECK(mgr2.load(&why));
  CHECK(mgr2.profiles().size() == 3);
  CHECK(mgr2.active().name == "weekend" && mgr2.active().gamertag == "Weekend Racer");
  CHECK(mgr2.find("default")->gamertag == "Colorado King");

  // Cannot remove default or active profile; removing inactive moves folder aside.
  CHECK(!mgr2.remove("default", &why));
  CHECK(!mgr2.remove("weekend", &why));
  write_file(mgr2.find("rally-run")->save_dir / "save.txt", "keep");
  CHECK(mgr2.remove("rally-run", &why));
  CHECK(mgr2.profiles().size() == 2 && !mgr2.find("rally-run"));
  CHECK(fs::exists(root / "profiles" / ".deleted-rally-run-1" / "save.txt"));
}

// ---------------------------------------------------------------- 3. Save Sync

profile::sync::Context make_sync_ctx(const fs::path& dev_root, const fs::path& sync_folder,
                                     const std::string& name, const std::string& build, int64_t now) {
  profile::sync::Context c;
  c.save_dir = dev_root / "drive";
  c.state_file = dev_root / "config" / "save_sync.toml";
  c.sync_folder = sync_folder;
  c.device_name = name;
  c.platform = name == "STEAMDECK" ? "linux" : "windows";
  c.build = build;
  c.backups.save_dir = c.save_dir;
  c.backups.backups_dir = dev_root / "backups" / "saves";
  c.backups.keep = 10;
  c.now = now;
  return c;
}

void test_save_sync() {
  const fs::path root = fresh("sync");
  const fs::path dev_a = root / "pc";
  const fs::path dev_b = root / "deck";
  const fs::path shared = root / "syncthing";

  // Missing sync folder is reported and never created.
  auto missing_ctx = make_sync_ctx(dev_a, root / "unplugged", "DESKTOP", "base", 1700100000);
  auto missing_rep = profile::sync::boot(missing_ctx);
  CHECK(missing_rep.enabled && !missing_rep.reachable && missing_rep.problem == "THE SYNC FOLDER IS NOT REACHABLE");
  CHECK(!fs::exists(root / "unplugged"));

  fs::create_directories(shared);

  // Device A has initial save -> publishes at boot.
  write_sample_save(dev_a / "drive", "NIS_SkylineGTRVSpecII_02", 50000);
  auto ctx_a = make_sync_ctx(dev_a, shared, "DESKTOP", "base", 1700100000);
  auto rep_a = profile::sync::boot(ctx_a);
  CHECK(rep_a.reachable && rep_a.published && !rep_a.imported && !rep_a.conflict);

  // Device B has no save -> auto-pulls Device A's save at boot.
  auto ctx_b = make_sync_ctx(dev_b, shared, "STEAMDECK", "base", 1700100010);
  auto rep_b = profile::sync::boot(ctx_b);
  CHECK(rep_b.reachable && rep_b.imported && rep_b.imported_from == "DESKTOP" && !rep_b.conflict);
  drive::DriveSave b_save;
  std::string err;
  CHECK(drive::read_save(dev_b / "drive" / "save.txt", b_save, &err) && b_save.credits == 50000);

  // Clean continuation: Device B advances and publishes -> Device A auto-pulls with before-sync backup!
  write_sample_save(dev_b / "drive", "NIS_SkylineGTRVSpecII_02", 75000);
  ctx_b.now = 1700100100;
  CHECK(profile::sync::publish(ctx_b, &err));

  ctx_a.now = 1700100200;
  rep_a = profile::sync::boot(ctx_a);
  CHECK(rep_a.imported && rep_a.imported_from == "STEAMDECK" && !rep_a.conflict);
  drive::DriveSave a_save;
  CHECK(drive::read_save(dev_a / "drive" / "save.txt", a_save, &err) && a_save.credits == 75000);
  auto a_backups = profile::backup::list(ctx_a.backups);
  CHECK(!a_backups.empty() && a_backups[0].reason == "before-sync" && a_backups[0].credits == 50000);

  // Divergence (conflict): both A and B edit independently!
  write_sample_save(dev_a / "drive", "NIS_SkylineGTRVSpecII_02", 80000);
  ctx_a.now = 1700100300;
  CHECK(profile::sync::publish(ctx_a, &err));

  write_sample_save(dev_b / "drive", "VW_Corrado_95", 90000);
  ctx_b.now = 1700100310;
  rep_b = profile::sync::boot(ctx_b);
  CHECK(!rep_b.imported && rep_b.conflict);
  CHECK(rep_b.remotes.size() == 1 && rep_b.remotes[0].relation == profile::sync::Relation::kDiverged);

  // Device A also sees divergence.
  ctx_a.now = 1700100320;
  rep_a = profile::sync::boot(ctx_a);
  CHECK(!rep_a.imported && rep_a.conflict);
  CHECK(rep_a.remotes.size() == 1 && rep_a.remotes[0].relation == profile::sync::Relation::kDiverged);

  // Resolve conflict with choose_keep on B: B's history absorbs A's hash, then A auto-pulls B cleanly!
  const std::string a_id = rep_b.remotes[0].m.device_id;
  ctx_b.now = 1700100400;
  CHECK(profile::sync::choose_keep(ctx_b, a_id, &err));
  ctx_a.now = 1700100410;
  rep_a = profile::sync::boot(ctx_a);
  CHECK(rep_a.imported && rep_a.imported_from == "STEAMDECK" && !rep_a.conflict);
  CHECK(drive::read_save(dev_a / "drive" / "save.txt", a_save, &err) && a_save.credits == 90000);

  // Test choose_load: diverge again, choose_load on A -> next boot of A pulls B.
  write_sample_save(dev_a / "drive", "NIS_SkylineGTRVSpecII_02", 95000);
  ctx_a.now = 1700100500;
  CHECK(profile::sync::publish(ctx_a, &err));
  write_sample_save(dev_b / "drive", "VW_Corrado_95", 110000);
  ctx_b.now = 1700100510;
  CHECK(profile::sync::publish(ctx_b, &err));
  ctx_a.now = 1700100520;
  rep_a = profile::sync::boot(ctx_a);
  CHECK(rep_a.conflict);
  const std::string b_id = rep_a.remotes[0].m.device_id;
  CHECK(profile::sync::choose_load(ctx_a, b_id, &err));
  ctx_a.now = 1700100530;
  rep_a = profile::sync::boot(ctx_a);
  CHECK(rep_a.imported && !rep_a.conflict);
  CHECK(drive::read_save(dev_a / "drive" / "save.txt", a_save, &err) && a_save.credits == 110000);

  // Sync-conflict copies and damaged archives are ignored safely.
  write_file(shared / "devices" / (b_id + ".sync-conflict-20261122.toml"), "format = 1\n");
  write_file(shared / "devices" / "0123456789abcdef0123456789abcdef.toml",
             "format = 1\ndevice_id = \"0123456789abcdef0123456789abcdef\"\ndevice_name = \"BAD\"\n"
             "platform = \"linux\"\nversion = \"base\"\nsave_hash = \"AAA\"\narchive_sha256 = \"BBB\"\narchive_bytes = 999\n");
  write_file(shared / "devices" / "0123456789abcdef0123456789abcdef.lsave", "truncated");
  profile::sync::DeviceState st_a;
  profile::sync::load_state(ctx_a.state_file, st_a);
  auto rems = profile::sync::remotes(ctx_a, st_a);
  bool saw_damaged_unloaded = false;
  for (const auto& r : rems)
    if (r.m.device_name == "BAD") saw_damaged_unloaded = !r.loadable && !r.problem.empty();
  CHECK(saw_damaged_unloaded);

  // v4 one-way guard: Device C publishes on v4 -> base Device A sees kNeedsV4 & never loads it.
  const fs::path dev_c = root / "v4pc";
  write_sample_save(dev_c / "drive", "NIS_SkylineGTRVSpecII_02", 500000);
  auto ctx_c = make_sync_ctx(dev_c, shared, "V4BOX", "v4", 1700100600);
  CHECK(profile::sync::boot(ctx_c).published);
  profile::sync::load_state(ctx_a.state_file, st_a);
  bool saw_v4_blocked = false;
  for (const auto& r : profile::sync::remotes(ctx_a, st_a))
    if (r.m.device_name == "V4BOX") saw_v4_blocked = (r.relation == profile::sync::Relation::kNeedsV4 && !r.loadable);
  CHECK(saw_v4_blocked);
}

// ---------------------------------------------------------------- 4. Pinyon / Xenia Import

// Builds a minimal valid big-endian ForzaProfile.body (FORMAT-PROFILE).
std::vector<uint8_t> build_synthetic_body(uint32_t version_flags, int32_t credits, int32_t xp) {
  std::vector<uint8_t> b;
  auto be32 = [&](uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
  };
  auto field_hdr = [&](const std::string& name, uint8_t type) {
    be32(uint32_t(name.size()));
    b.insert(b.end(), name.begin(), name.end());
    be32(0x20);
    be32(0);
    b.push_back(type);
  };
  // Top-level: 2 fields: VersionFlags (u32) + Main (struct with 3 fields).
  be32(2);
  field_hdr("VersionFlags", 0x03);
  be32(version_flags);
  field_hdr("Main", 0x0F);
  be32(3);
  field_hdr("Credits", 0x07);
  be32(uint32_t(credits));
  field_hdr("XP", 0x07);
  be32(uint32_t(xp));
  field_hdr("CareerCar", 0x07);
  be32(1);
  // Terminator padding word (0xBB).
  be32(0xBBBBBBBBu);
  return b;
}

void build_synthetic_player_db(const fs::path& db_path) {
  std::error_code ec;
  fs::create_directories(db_path.parent_path(), ec);
  fs::remove(db_path, ec);
  sqlite3* db = nullptr;
  CHECK(sqlite3_open(db_path.string().c_str(), &db) == SQLITE_OK);
  const char* sql =
      "CREATE TABLE Career_Garage (Id INTEGER PRIMARY KEY, CarId INTEGER, PerformanceIndex REAL, ClassID INTEGER, "
      "DefaultManufacturerColorIndex INTEGER);"
      "INSERT INTO Career_Garage VALUES (1, 445, 0.461, 3, 0);"
      "CREATE TABLE Career_PurchasedParts (GarageId INTEGER, PartEnum INTEGER, PartId INTEGER, ManufacturerId INTEGER, PricePaid INTEGER);"
      "CREATE TABLE Career_EventResults (EventId INTEGER, Place INTEGER);"
      "CREATE TABLE Career_RaceResults (RaceId INTEGER, Place INTEGER);"
      "CREATE TABLE CareerEventUnlocks (EventId INTEGER);"
      "CREATE TABLE CareerHubUnlocks (HubId INTEGER);"
      "CREATE TABLE LastUsedGarageId (Id INTEGER);"
      "INSERT INTO LastUsedGarageId VALUES (1);";
  CHECK(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

void write_stored_zip(const fs::path& zip, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> out, cd;
  auto le16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x & 0xFF)); v.push_back(uint8_t(x >> 8)); };
  auto le32 = [&](std::vector<uint8_t>& v, uint32_t x) { le16(v, uint16_t(x & 0xFFFF)); le16(v, uint16_t(x >> 16)); };
  for (const auto& [name, data] : files) {
    const uint32_t crc = fh1::crc32(data);
    const uint32_t off = uint32_t(out.size());
    le32(out, 0x04034B50); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
    le32(out, crc); le32(out, uint32_t(data.size())); le32(out, uint32_t(data.size()));
    le16(out, uint16_t(name.size())); le16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());

    le32(cd, 0x02014B50); le16(cd, 20); le16(cd, 20); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0);
    le32(cd, crc); le32(cd, uint32_t(data.size())); le32(cd, uint32_t(data.size()));
    le16(cd, uint16_t(name.size())); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, off);
    cd.insert(cd.end(), name.begin(), name.end());
  }
  const uint32_t cd_off = uint32_t(out.size());
  out.insert(out.end(), cd.begin(), cd.end());
  le32(out, 0x06054B50); le16(out, 0); le16(out, 0);
  le16(out, uint16_t(files.size())); le16(out, uint16_t(files.size()));
  le32(out, uint32_t(cd.size())); le32(out, cd_off); le16(out, 0);
  write_bytes(zip, out);
}

void test_xenia_pinyon_import() {
  const fs::path root = fresh("import");

  // (a) Encrypted Xenia / Pinyon folder -> detected & refused with zero-key export message.
  const fs::path xenia_dir = root / "xenia" / "content" / "E000000000000001" / "4D5309C9" / "00000001" / "ForzaProfile";
  write_file(xenia_dir / "ForzaProfile", "ENCRYPTED_XBOX360_BYTES");
  auto det_enc = profile::import::detect(root / "xenia", root / "work");
  CHECK(det_enc.kind == profile::import::SourceKind::kEncryptedProfile);
  CHECK(det_enc.problem.find("no console keys") != std::string::npos);

  // (b) Plaintext export folder (base save, version_flags = 0), nested under user/.
  const fs::path exp_root = root / "export_root";
  write_bytes(exp_root / "user" / "ForzaProfile.body", build_synthetic_body(0, 123456, 2500));
  build_synthetic_player_db(exp_root / "user" / "PlayerDatabase.sqlite");

  const fs::path save_dir = root / "dest" / "drive";
  write_sample_save(save_dir, "VW_Corrado_95", 500);  // existing save to preserve as .pre-import

  profile::import::Options opts;
  opts.save_dir = save_dir;
  opts.v4_available = false;
  opts.backups.save_dir = save_dir;
  opts.backups.backups_dir = root / "dest" / "backups" / "saves";
  opts.media_of = [](int64_t id) { return id == 445 ? std::string("NIS_SkylineGTRVSpecII_02") : std::string(); };

  auto res = profile::import::run(exp_root, opts);
  CHECK(res.ok && !res.v4_save && res.credits == 123456 && res.garage_cars == 1 && res.car == "NIS_SkylineGTRVSpecII_02");
  CHECK(fs::exists(save_dir / "save.txt.pre-import"));
  CHECK(fs::exists(save_dir / "garage.txt.pre-import"));
  CHECK(!profile::backup::list(opts.backups).empty() && profile::backup::list(opts.backups)[0].reason == "before-import");

  // Re-importing over an already-imported career refuses unless replace_existing = true.
  auto res_dup = profile::import::run(exp_root, opts);
  CHECK(!res_dup.ok && res_dup.error.find("already holds") != std::string::npos);

  // (c) ZIP archive import with v4 save (version_flags = 2).
  const fs::path v4_dir = root / "v4_exp";
  write_bytes(v4_dir / "ForzaProfile.body", build_synthetic_body(2, 777000, 9000));
  build_synthetic_player_db(v4_dir / "PlayerDatabase.sqlite");
  std::vector<uint8_t> db_bytes;
  {
    std::ifstream f(v4_dir / "PlayerDatabase.sqlite", std::ios::binary);
    db_bytes.assign(std::istreambuf_iterator<char>(f), {});
  }
  const std::string v4_export_json = "{\"VersionFlags\": 2}\n";
  const fs::path zip_path = root / "pinyon_export_v4.zip";
  write_stored_zip(zip_path, {{"user/ForzaProfile.body", build_synthetic_body(2, 777000, 9000)},
                              {"user/PlayerDatabase.sqlite", db_bytes},
                              {"user/export.json", std::vector<uint8_t>(v4_export_json.begin(), v4_export_json.end())}});

  opts.replace_existing = true;
  opts.v4_available = false;
  auto res_v4_blocked = profile::import::run(zip_path, opts);
  CHECK(!res_v4_blocked.ok && res_v4_blocked.v4_save && res_v4_blocked.error.find("v4") != std::string::npos);

  opts.v4_available = true;
  auto res_v4_ok = profile::import::run(zip_path, opts);
  CHECK(res_v4_ok.ok && res_v4_ok.v4_save && res_v4_ok.credits == 777000);
  CHECK(res_v4_ok.detection.kind == profile::import::SourceKind::kExportZip);

  // (d) P0-G4: importing into an existing save preserves spawn pose, camera/audio, and controls!
  {
    const fs::path keep_dir = root / "dest_preserve" / "drive";
    drive::DriveSave existing;
    existing.car = "VW_Corrado_95";
    existing.position = {1234.5, 56.25, -789.0};
    existing.heading = 1.75;
    existing.has_spawn = true;
    existing.spawn = {111.0, 22.0, 333.0};
    existing.spawn_heading = -0.85;
    existing.radio_station = "Radio2_BassArena";
    existing.radio_dj = false;
    existing.radio_volume = 0.65;
    existing.tod_sec = 43210.0;
    existing.has_controls = true;
    existing.controls.layout_id = 3;
    existing.controls.rumble_enabled = false;
    existing.controls.rumble_scale = 0.4;
    existing.controls.camera_effects = false;
    existing.controls.assists.steering = 2;
    existing.controls.assists.shifting = 2;
    std::string werr;
    CHECK(drive::write_save(keep_dir / "save.txt", existing, &werr));

    profile::import::Options keep_opts = opts;
    keep_opts.save_dir = keep_dir;
    keep_opts.backups.save_dir = keep_dir;
    keep_opts.backups.backups_dir = root / "dest_preserve" / "backups";
    keep_opts.replace_existing = true;
    auto rkeep = profile::import::run(exp_root, keep_opts);
    CHECK(rkeep.ok && rkeep.credits == 123456);

    drive::DriveSave after;
    CHECK(drive::read_save(keep_dir / "save.txt", after, &werr));
    CHECK(after.credits == 123456 && after.owned_garage);
    CHECK(std::fabs(after.position[0] - 1234.5) < 1e-3 && std::fabs(after.position[2] - (-789.0)) < 1e-3);
    CHECK(std::fabs(after.heading - 1.75) < 1e-3);
    CHECK(after.has_spawn && std::fabs(after.spawn[0] - 111.0) < 1e-3 && std::fabs(after.spawn_heading - (-0.85)) < 1e-3);
    CHECK(after.has_controls && after.controls.layout_id == 3 && !after.controls.rumble_enabled &&
          std::fabs(after.controls.rumble_scale - 0.4) < 1e-3 && !after.controls.camera_effects &&
          after.controls.assists.steering == 2 && after.controls.assists.shifting == 2);
    CHECK(after.radio_station == "Radio2_BassArena" && !after.radio_dj && std::fabs(after.radio_volume - 0.65) < 1e-3);
    CHECK(std::fabs(after.tod_sec - 43210.0) < 1e-2);
  }
}

void test_liveries_backup_and_sync() {
  // P0-G3: liveries/*.liv are included in backup snapshots/restore and cross-device sync!
  const fs::path root = fresh("liveries_g3");
  const fs::path dev_a = root / "pc_a";
  const fs::path dev_b = root / "pc_b";
  const fs::path shared = root / "shared";
  fs::create_directories(shared);

  write_sample_save(dev_a / "drive", "NIS_SkylineGTRVSpecII_02", 42000);
  const std::string liv_payload = "LIV1_CUSTOM_DECAL_PAYLOAD_DATA";
  write_file(dev_a / "drive" / "liveries" / "skyline_custom.liv", liv_payload);

  // 1) Backup snapshot & restore preserves liveries/*.liv.
  profile::backup::Config bcfg;
  bcfg.save_dir = dev_a / "drive";
  bcfg.backups_dir = dev_a / "backups";
  profile::backup::Entry snap;
  std::string err;
  CHECK(profile::backup::snapshot(bcfg, "manual", &snap, &err, 1700200000));
  CHECK(fs::exists(snap.dir / "liveries" / "skyline_custom.liv"));

  // Delete livery folder on disk, restore backup -> livery file comes back byte-for-byte!
  std::error_code ec;
  fs::remove_all(dev_a / "drive" / "liveries", ec);
  CHECK(!fs::exists(dev_a / "drive" / "liveries" / "skyline_custom.liv"));
  CHECK(profile::backup::restore(bcfg, snap.id, &err, 1700200010));
  CHECK(fs::exists(dev_a / "drive" / "liveries" / "skyline_custom.liv"));
  {
    std::ifstream in(dev_a / "drive" / "liveries" / "skyline_custom.liv", std::ios::binary);
    std::string restored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(restored == liv_payload);
  }

  // 2) Sync publish from A -> boot on B brings liveries/*.liv across!
  const std::string h1 = profile::sync::save_hash(dev_a / "drive");
  write_file(dev_a / "drive" / "liveries" / "skyline_custom.liv", liv_payload + "_V2");
  const std::string h2 = profile::sync::save_hash(dev_a / "drive");
  CHECK(!h1.empty() && !h2.empty() && h1 != h2);

  auto ctx_a = make_sync_ctx(dev_a, shared, "PCA", "v4", 1700200100);
  auto rep_a = profile::sync::boot(ctx_a);
  CHECK(rep_a.published);

  auto ctx_b = make_sync_ctx(dev_b, shared, "PCB", "v4", 1700200110);
  auto rep_b = profile::sync::boot(ctx_b);
  CHECK(rep_b.imported && rep_b.imported_from == "PCA");
  CHECK(fs::exists(dev_b / "drive" / "liveries" / "skyline_custom.liv"));
  {
    std::ifstream in(dev_b / "drive" / "liveries" / "skyline_custom.liv", std::ios::binary);
    std::string synced((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(synced == liv_payload + "_V2");
  }
}

}  // namespace

int main() {
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / "shift_save_ecosystem_tests";
  fs::remove_all(g_root, ec);
  fs::create_directories(g_root, ec);

  test_rolling_backups();
  test_profiles();
  test_save_sync();
  test_xenia_pinyon_import();
  test_liveries_backup_and_sync();

  fs::remove_all(g_root, ec);
  if (g_failed) {
    std::fprintf(stderr, "save_ecosystem_tests: %d failed\n", g_failed);
    return 1;
  }
  std::printf("save_ecosystem_tests: all passed\n");
  return 0;
}
