// Importing an original profile resumes where it was (PROG-13, PROG-14,
// PROG-15), synthetic cases: the SponsorshipState codec (popularity and the
// sponsors), the Main fields that exist (MaxSkillsCarOrdinal, Level,
// LastRewardedLevel, WristbandLevel) and none that do not, the opening
// stage, the campaign mirror an import rebuilds (bosses, showcases, the
// finale, heard dialogue: nothing queued again), the kept driver level, and
// an export that round-trips the same fields.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "drive/save.h"
#include "freeroam/skills.h"
#include "garage/garage.h"
#include "profile/drive_save.h"
#include "profile/profile.h"
#include "profile/xenia_import.h"
#include "race/campaign_flow.h"
#include "race/rewards.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

struct Bytes {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u32(uint32_t v) {
    b.push_back(uint8_t(v >> 24)), b.push_back(uint8_t(v >> 16)), b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v));
  }
  void str(const std::string& s) {
    u32(uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
  void field(const std::string& name, uint8_t type) {
    str(name);
    u32(0x20), u32(0), u8(type);
  }
  void record(const std::string& name, const std::string& cls, uint32_t ver, const std::vector<uint8_t>& payload) {
    str(name), str(cls), u32(ver), u32(uint32_t(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
  }
};

// A SponsorshipState payload laid out like the reference saves': points, a
// zero word, a f32, 2, the count, then (tiers, count) children.
std::vector<uint8_t> sponsorship_payload(uint32_t points, float f,
                                         const std::vector<profile::SponsorshipChallenge>& kids) {
  Bytes p;
  p.u32(points), p.u32(0);
  uint32_t fb = 0;
  std::memcpy(&fb, &f, 4);
  p.u32(fb), p.u32(2), p.u32(uint32_t(kids.size()));
  for (const auto& k : kids) {
    Bytes c;
    c.u32(k.tiers_claimed), c.u32(k.count);
    p.record(k.id, "CSponsorshipChallengeState", 1, c.b);
  }
  return p.b;
}

std::vector<uint8_t> build_body(int32_t xp, int32_t wristband, uint32_t level, uint32_t ftc_stage,
                                const std::vector<uint8_t>& sponsorship) {
  Bytes b;
  b.u32(2);
  b.field("VersionFlags", 0x03), b.u32(0);
  b.field("Main", 0x0F), b.u32(8);
  b.field("Credits", 0x03), b.u32(250000);
  b.field("XP", 0x07), b.u32(uint32_t(xp));
  b.field("WristbandLevel", 0x07), b.u32(uint32_t(wristband));
  b.field("Level", 0x03), b.u32(level);
  b.field("LastRewardedLevel", 0x03), b.u32(level);
  b.field("MaxSkillsScore", 0x03), b.u32(17515);
  b.field("MaxSkillsCarOrdinal", 0x03), b.u32(633);
  b.field("CareerCar", 0x07), b.u32(1);
  // Block 1: Colorado (the first-time career child) and SponsorshipState.
  b.u32(2), b.u32(sponsorship.empty() ? 1 : 2);
  Bytes colorado;
  colorado.u32(1);
  Bytes ftc;
  ftc.u8(1), ftc.u32(ftc_stage);
  colorado.record("first_time_career_activity", "CFirstTimeCareerState", 1, ftc.b);
  b.record("Colorado", "CHorizonLocationState", 2, colorado.b);
  if (!sponsorship.empty()) b.record("SponsorshipState", "CHorizonSponsorshipState", 2, sponsorship);
  return b.b;
}

void write_export(const fs::path& dir, const std::vector<uint8_t>& body,
                  const std::vector<std::pair<int64_t, int>>& results, const std::vector<int64_t>& unlocks) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  std::ofstream(dir / "ForzaProfile.body", std::ios::binary).write(reinterpret_cast<const char*>(body.data()),
                                                                   std::streamsize(body.size()));
  sqlite3* db = nullptr;
  CHECK(sqlite3_open((dir / "PlayerDatabase.sqlite").string().c_str(), &db) == SQLITE_OK);
  std::string sql =
      "CREATE TABLE Career_Garage (Id INTEGER PRIMARY KEY, CarId INTEGER, PerformanceIndex REAL, ClassID INTEGER, "
      "DefaultManufacturerColorIndex INTEGER);"
      "INSERT INTO Career_Garage VALUES (1, 445, 0.461, 3, 0);"
      "CREATE TABLE Career_PurchasedParts (GarageId INTEGER, PartEnum INTEGER, PartId INTEGER, ManufacturerId "
      "INTEGER, PricePaid INTEGER);"
      "CREATE TABLE Career_EventResults (EventId INTEGER, Place INTEGER);"
      "CREATE TABLE Career_RaceResults (RaceId INTEGER, Place INTEGER);"
      "CREATE TABLE CareerEventUnlocks (EventId INTEGER);"
      "CREATE TABLE CareerHubUnlocks (HubId INTEGER);"
      "CREATE TABLE LastUsedGarageId (Id INTEGER);"
      "INSERT INTO LastUsedGarageId VALUES (1);";
  for (const auto& [id, place] : results)
    sql += "INSERT INTO Career_EventResults VALUES (" + std::to_string(id) + "," + std::to_string(place) + ");";
  for (int64_t id : unlocks) sql += "INSERT INTO CareerEventUnlocks VALUES (" + std::to_string(id) + ");";
  CHECK(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

fs::path scratch(const char* name) {
  const fs::path p = fs::temp_directory_path() / "shift_save_import_tests" / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

const profile::Record* find_record(const profile::Imported& p, const char* name) {
  for (const auto& blk : p.body.blocks)
    for (const auto& r : blk.records)
      if (r.name == name) return &r;
  return nullptr;
}

std::string no_media(int64_t) { return {}; }

const std::vector<profile::SponsorshipChallenge> kUserSponsors = {
    {"Air", 3, 159}, {"Drift", 9, 715}, {"Fame", 5, 0}, {"Over9000", 0, 1}};

// ---- 1. The SponsorshipState codec.
void test_sponsorship_codec() {
  const auto payload = sponsorship_payload(811260, 246.66f, kUserSponsors);
  profile::SponsorshipState st;
  CHECK(profile::decode_sponsorship_state(payload, st));
  CHECK(st.popularity_points == 811260);
  CHECK(profile::popularity_points_from_sponsorship_state(payload) == 811260);
  CHECK(st.challenges.size() == 4);
  CHECK(st.find("Fame") && st.find("Fame")->tiers_claimed == 5 && st.find("Fame")->count == 0);
  CHECK(st.find("Drift") && st.find("Drift")->tiers_claimed == 9 && st.find("Drift")->count == 715);
  // Unchanged values: byte-identical.
  CHECK(profile::encode_sponsorship_state(payload, st) == payload);
  // Patched values: only those words move; an id the record lacks is appended.
  profile::SponsorshipState up = st;
  up.popularity_points = 1689100;
  up.challenges[1].tiers_claimed = 10, up.challenges[1].count = 800;
  up.challenges.push_back({"King", 1, 7});
  const auto patched = profile::encode_sponsorship_state(payload, up);
  CHECK(patched.size() > payload.size());
  CHECK(std::equal(payload.begin() + 4, payload.begin() + 16, patched.begin() + 4));  // f32 and words kept
  profile::SponsorshipState back;
  CHECK(profile::decode_sponsorship_state(patched, back));
  CHECK(back.popularity_points == 1689100 && back.challenges.size() == 5);
  CHECK(back.find("Drift")->tiers_claimed == 10 && back.find("Drift")->count == 800);
  CHECK(back.find("King") && back.find("King")->count == 7);
  // A fresh record: every SponsorshipChallenges.xml id.
  const auto fresh = profile::encode_sponsorship_state({}, up);
  profile::SponsorshipState f;
  CHECK(profile::decode_sponsorship_state(fresh, f));
  CHECK(f.popularity_points == 1689100 && f.challenges.size() == 15);
  CHECK(f.find("Fame")->tiers_claimed == 5 && f.find("King")->tiers_claimed == 1);
  CHECK(!profile::decode_sponsorship_state(std::vector<uint8_t>{1, 2, 3}, f));
  // A layout the reader does not know is kept as is.
  const std::vector<uint8_t> odd = {1, 2, 3};
  CHECK(profile::encode_sponsorship_state(odd, up) == odd);
}

// ---- 2. Import: the real Main fields and SponsorshipState.
void test_import_reads_saved_progress() {
  const fs::path dir = scratch("import");
  const auto spons = sponsorship_payload(811260, 246.66f, kUserSponsors);
  write_export(dir, build_body(3080, 4, 11, 8, spons), {{79, 1}, {167, 1}}, {101, 223});
  profile::Imported p;
  std::string err;
  CHECK(profile::import_profile(dir, p, &err));
  CHECK(p.has_sponsorship && p.popularity == 811260.0 && p.fame_tiers_claimed == 5 && p.sponsors.size() == 4);
  CHECK(p.max_skills_score == 17515 && p.max_skills_car == 633);
  CHECK(p.level == 11 && p.last_rewarded_level == 11 && p.wristband_level == 4);
  CHECK(p.has_first_time_career && p.first_time_career_stage == 8);

  drive::DriveSave ds;
  profile::to_drive_save(p, no_media, ds, nullptr);
  CHECK(ds.popularity_points == 811260.0);
  CHECK(ds.fame_rewards_claimed == 5);
  CHECK(ds.sponsors.size() == 3);  // Fame goes to fame_rewards_claimed
  bool drift_ok = false;
  for (const auto& sp : ds.sponsors) drift_ok = drift_ok || (sp.id == "Drift" && sp.count == 715 && sp.claimed == 9);
  CHECK(drift_ok);
  CHECK(ds.max_skills_car == 633 && ds.max_skills_score == 17515);
  CHECK(ds.level == 12);  // PlayerLevelDef Id 11 is the engine's level 12
  CHECK(ds.driver_xp == 0);
  CHECK(ds.collected_wristband == 4 && ds.intro_completed && ds.campaign_stage == "freeroam");
  CHECK(ds.wristband_points == 3080);
}

// ---- 3. The opening and the wristband mapping.
void test_career_stage() {
  profile::Imported p;
  p.has_first_time_career = true, p.first_time_career_stage = 1;
  auto s = profile::imported_career_stage(p);
  CHECK(!s.intro_completed && s.campaign_stage == "viper_drive" && s.collected_wristband == -1);
  p.first_time_career_stage = 2;
  s = profile::imported_career_stage(p);
  CHECK(!s.intro_completed && s.campaign_stage == "qualify");
  p.first_time_career_stage = 8, p.wristband_level = 0;
  s = profile::imported_career_stage(p);
  CHECK(s.intro_completed && s.collected_wristband == 0);
  // A band earned (1,500 points: pink) but only blue collected stays to collect.
  p.wristband_level = 2, p.xp = 1500;
  s = profile::imported_career_stage(p);
  CHECK(s.collected_wristband == 2);
  p.wristband_level = 9;
  CHECK(profile::imported_career_stage(p).collected_wristband == 6);
  CHECK(profile::engine_driver_level_from_profile(0) == 1 && profile::profile_level_from_engine(1) == 0);
  CHECK(profile::profile_level_from_engine(0) == 0 && profile::engine_driver_level_from_profile(11) == 12);
}

// ---- 4. The campaign mirror.
size_t pending_dialog(const race::CampaignFlow& f) { return f.dialogs().queued_count() + (f.dialogs().active() ? 1 : 0); }

void test_campaign_inference() {
  // (a) A user-like save: orange collected, 3,080 points, rank 115.
  race::CampaignImportInputs in;
  in.event_results = {{77, 1}, {79, 1}, {101, 1}, {168, 1}, {211, 1}, {223, 1}, {241, 1}, {243, 1}};
  in.wristband_points = 3080, in.collected_wristband = 4, in.popularity_rank = 115;
  auto st = race::infer_campaign_state(in);
  const std::set<std::string> bosses(st.unlocked_bosses.begin(), st.unlocked_bosses.end());
  CHECK(bosses == std::set<std::string>({"ramona", "ali", "duke", "marko"}));
  const std::set<std::string> sc(st.unlocked_showcases.begin(), st.unlocked_showcases.end());
  CHECK(sc.size() == 6 && sc.count("PLANE_RACE_001") && sc.count("EXHIBITION_RESE_003") && !sc.count("PLANE_RACE_003"));
  CHECK(!st.finale_complete);
  // Nothing is queued again: not the boss unlocks, not the showcase unlocks.
  {
    race::CampaignFlow flow;
    flow.load_from(st);
    flow.on_wristband_points_changed(in.wristband_points, in.collected_wristband);
    flow.on_popularity_rank_changed(116, 115);
    CHECK(pending_dialog(flow) == 0);
    // The same calls on an empty mirror do queue lines (the check bites).
    race::CampaignFlow bare;
    bare.load_from({});
    bare.on_wristband_points_changed(in.wristband_points, in.collected_wristband);
    bare.on_popularity_rank_changed(116, 115);
    CHECK(pending_dialog(bare) > 0);
  }
  // A climb past the next gate still unlocks it, once.
  {
    race::CampaignFlow flow;
    flow.load_from(st);
    flow.on_popularity_rank_changed(115, 100);
    CHECK(flow.unlocked_showcases().count("PLANE_RACE_003") == 1);
    CHECK(pending_dialog(flow) == 1);
  }

  // (b) HEADLINE_EVENT won, Darius not yet beaten: the showdown is pending.
  race::CampaignImportInputs hd;
  hd.event_results = {{167, 1}, {245, 1}};
  hd.wristband_points = 12000, hd.collected_wristband = 6, hd.popularity_rank = 28;
  st = race::infer_campaign_state(hd);
  CHECK(!st.finale_complete);
  CHECK(std::count(st.unlocked_bosses.begin(), st.unlocked_bosses.end(), "darius") == 1);
  {
    race::CampaignFlow flow;
    flow.load_from(st);
    CHECK(flow.pending_darius_showdown() && !flow.finale_complete());
    CHECK(flow.next_objective(12000, 6, 28).title.find("DARIUS") != std::string::npos);
  }
  // (c) NEM_FINAL won: the finale is complete.
  hd.event_results.push_back({169, 1});
  st = race::infer_campaign_state(hd);
  CHECK(st.finale_complete);
  {
    race::CampaignFlow flow;
    flow.load_from(st);
    CHECK(flow.finale_complete() && !flow.pending_darius_showdown());
    flow.on_wristband_points_changed(12000, 6);
    CHECK(pending_dialog(flow) == 0);
  }
  // (d) A band earned but not collected: the ceremony is still the objective.
  race::CampaignImportInputs pend;
  pend.wristband_points = 1500, pend.collected_wristband = 2, pend.popularity_rank = 250;
  st = race::infer_campaign_state(pend);
  {
    race::CampaignFlow flow;
    flow.load_from(st);
    CHECK(flow.next_objective(1500, 2, 250).title == "COLLECT PINK WRISTBAND");
    CHECK(flow.unlocked_bosses().count("duke") == 1 && flow.unlocked_bosses().count("marko") == 0);
  }
  // (e) No rank: showcases come from the stored unlocks.
  race::CampaignImportInputs nr;
  nr.event_unlocks = {101, 223, 59};
  st = race::infer_campaign_state(nr);
  CHECK(st.unlocked_showcases.size() == 3);

  // seed_imported_campaign merges into a DriveSave.
  drive::DriveSave ds;
  ds.event_results = {{167, 1}, {169, 1}};
  ds.wristband_points = 12580, ds.collected_wristband = 6;
  ds.heard_dialogs = {"barn_found_barnfind_01"};
  race::seed_imported_campaign(ds, 28);
  CHECK(ds.finale_complete && ds.unlocked_showcases.size() == 9 && ds.unlocked_bosses.size() == 7);
  CHECK(std::count(ds.heard_dialogs.begin(), ds.heard_dialogs.end(), "barn_found_barnfind_01") == 1);
  CHECK(std::count(ds.heard_dialogs.begin(), ds.heard_dialogs.end(), "wb_collect_gold") == 1);
}

// ---- 5. The driver level is kept as saved.
void test_driver_level() {
  race::ProgressionTables t;
  const int xp[] = {0, 5000, 10000, 18000, 26000, 35000, 44000, 53000, 63000, 73000, 84000, 95000, 107000};
  for (int i = 0; i < 13; ++i) t.driver_levels.push_back({i, xp[i], 5});
  int dxp = 0;
  CHECK(race::resume_driver_level(&t, 12, dxp) == 12 && dxp == 95000);  // Main/Level 11
  dxp = 20000;
  CHECK(race::resume_driver_level(&t, 1, dxp) == 4 && dxp == 20000);  // XP above the saved level wins
  dxp = 0;
  CHECK(race::resume_driver_level(&t, 1, dxp) == 1 && dxp == 0);
  dxp = 0;
  CHECK(race::resume_driver_level(nullptr, 7, dxp) == 7);
  CHECK(race::resume_driver_level(nullptr, 0, dxp) == 1);
}

// ---- 6. Export round-trips the same fields (template and none).
void test_export_round_trip() {
  const fs::path dir = scratch("rt_src");
  const auto spons = sponsorship_payload(1689100, 26.79f,
                                         {{"Air", 5, 313}, {"Drift", 10, 800}, {"Fame", 8, 0}, {"King", 0, 0}});
  write_export(dir, build_body(12580, 6, 0, 8, spons), {{167, 1}, {169, 1}}, {101});
  profile::Imported src;
  std::string err;
  CHECK(profile::import_profile(dir, src, &err));
  drive::DriveSave ds;
  profile::to_drive_save(src, no_media, ds, nullptr);
  const garage::Garage g = profile::to_garage(src);

  // (a) With the import as template: no invented Main fields (the corpus
  // test checks the 124/134 count on the real saves), SponsorshipState
  // byte-identical, every field equal.
  profile::Imported out;
  profile::from_drive_save(ds, g, out, &src);
  CHECK(!profile::find(out.body.fields, "Main/Popularity") && !profile::find(out.body.fields, "Main/DriverXP") &&
        !profile::find(out.body.fields, "Main/MaxSkillsCar"));
  CHECK(find_record(out, "SponsorshipState") && find_record(out, "SponsorshipState")->payload == spons);
  const fs::path rt = scratch("rt_out");
  CHECK(profile::export_forza_profile(ds, g, rt, &err, {dir, nullptr, -1}));
  profile::Imported back;
  CHECK(profile::import_profile(rt, back, &err));
  CHECK(back.popularity == src.popularity && back.fame_tiers_claimed == 8 && back.sponsors.size() == 4);
  CHECK(back.level == 0 && back.last_rewarded_level == 0 && back.wristband_level == 6 && back.xp == 12580);
  CHECK(back.max_skills_score == 17515 && back.max_skills_car == 633);
  CHECK(find_record(back, "SponsorshipState")->payload == spons);
  drive::DriveSave ds2;
  profile::to_drive_save(back, no_media, ds2, nullptr);
  CHECK(ds2.popularity_points == ds.popularity_points && ds2.fame_rewards_claimed == ds.fame_rewards_claimed);
  CHECK(ds2.sponsors.size() == ds.sponsors.size() && ds2.level == ds.level);
  CHECK(ds2.collected_wristband == ds.collected_wristband && ds2.max_skills_car == ds.max_skills_car);

  // (b) A progressed save with no template: a fresh SponsorshipState carries it.
  ds.popularity_points = 2042782.0, ds.fame_rewards_claimed = 10, ds.level = 12;
  const fs::path fresh = scratch("rt_fresh");
  CHECK(profile::export_forza_profile(ds, g, fresh, &err));
  profile::Imported fb;
  CHECK(profile::import_profile(fresh, fb, &err));
  CHECK(fb.has_sponsorship && fb.popularity == 2042782.0 && fb.fame_tiers_claimed == 10 && fb.sponsors.size() == 15);
  CHECK(fb.level == 11);
  CHECK(!profile::find(fb.body.fields, "Main/Popularity") && !profile::find(fb.body.fields, "Main/DriverXP"));
}

// ---- 7. The rank a popularity reaches gates the showcases (Fame ladder),
// and the Fame tiers already claimed are not paid again.
freeroam::SkillsData synthetic_ladder() {
  freeroam::SkillsData sk;
  // A ladder of 249 targets like Fame.xml's shape: rank r at (250 - r) * 10,000.
  for (int r = 249; r >= 1; --r) sk.fame_levels.push_back({r, double(250 - r) * 10000.0, "", ""});
  for (int req : {225, 200, 175, 150, 125, 100, 75, 50, 25, 1}) sk.fame_tiers.push_back({req, 5000});
  return sk;
}

size_t fame_payouts(const std::vector<freeroam::SkillEvent>& evs) {
  return size_t(std::count_if(evs.begin(), evs.end(), [](const freeroam::SkillEvent& e) {
    return e.kind == freeroam::SkillEvent::kFameSponsor;
  }));
}

void test_rank_from_points() {
  const freeroam::SkillsData sk = synthetic_ladder();
  CHECK(sk.rank_for_points(0) == 250);
  CHECK(sk.rank_for_points(1350000) == 115);
  race::CampaignImportInputs in;
  in.popularity_rank = sk.rank_for_points(1350000);
  const auto st = race::infer_campaign_state(in);
  CHECK(st.unlocked_showcases.size() == 6);
  // Rank 115 with its 5 tiers claimed: the next rank-up pays nothing.
  freeroam::SkillTracker kept(sk, 1355000.0, 5);
  const auto evs = kept.add_popularity(10000.0, 1.0);
  CHECK(kept.popularity_rank() == 114 && fame_payouts(evs) == 0 && kept.fame_rewards_claimed() == 5);
  // With the tiers lost (the old import), the same rank-up re-pays all five.
  freeroam::SkillTracker lost(sk, 1355000.0, 0);
  CHECK(fame_payouts(lost.add_popularity(10000.0, 1.0)) == 5);
}

// ---- 8. The in-app import writes save.txt; its reload rebuilds the mirror.
void test_import_run_then_reload() {
  const fs::path src = scratch("run_src");
  const auto spons = sponsorship_payload(1689100, 26.79f, {{"Drift", 10, 800}, {"Fame", 8, 0}});
  write_export(src, build_body(12580, 6, 0, 8, spons), {{167, 1}, {169, 1}, {79, 1}}, {101, 59});
  const fs::path saves = scratch("run_saves");
  profile::import::Options opts;
  opts.save_dir = saves;
  const auto res = profile::import::run(src, opts);
  CHECK(res.ok);
  drive::DriveSave disk;
  std::string err;
  CHECK(drive::read_save(saves / "save.txt", disk, &err));
  CHECK(disk.owned_garage && disk.popularity_points == 1689100.0 && disk.fame_rewards_claimed == 8);
  CHECK(disk.collected_wristband == 6 && disk.intro_completed && disk.level == 1);
  CHECK(disk.sponsors.size() == 1 && disk.sponsors[0].id == "Drift" && disk.sponsors[0].claimed == 10);
  // What CampaignDirector::load_from_save does for a not yet seeded import.
  CHECK(race::imported_campaign_needs_seed(disk));
  const int rank = synthetic_ladder().rank_for_points(1689100.0);
  race::seed_imported_campaign(disk, rank);
  CHECK(disk.finale_complete && disk.unlocked_bosses.size() == 7);
  CHECK(!race::imported_campaign_needs_seed(disk));
  race::CampaignFlow flow;
  race::CampaignSaveState st;
  st.campaign_stage = disk.campaign_stage, st.completed_cutscenes = disk.completed_cutscenes;
  st.heard_dialogs = disk.heard_dialogs, st.unlocked_bosses = disk.unlocked_bosses;
  st.unlocked_showcases = disk.unlocked_showcases, st.finale_complete = disk.finale_complete;
  flow.load_from(st);
  flow.on_wristband_points_changed(disk.wristband_points, disk.collected_wristband);
  flow.on_popularity_rank_changed(rank + 1, rank);
  CHECK(flow.finale_complete() && pending_dialog(flow) == 0);
  // Seeding twice changes nothing (every load path seeds).
  drive::DriveSave again = disk;
  race::seed_imported_campaign(again, rank);
  CHECK(again.heard_dialogs == disk.heard_dialogs && again.unlocked_showcases == disk.unlocked_showcases);
  CHECK(again.unlocked_bosses == disk.unlocked_bosses && again.completed_cutscenes == disk.completed_cutscenes);
}

// ---- 9. An engine career with a prize car also has owned_garage; its own
// mirror is kept, so an unlock still pending at save time plays on reload.
void test_engine_career_not_seeded() {
  drive::DriveSave native;
  native.owned_garage = true, native.career_started = true, native.intro_completed = true;
  native.campaign_stage = "freeroam";
  native.wristband_points = 3080, native.collected_wristband = 4;
  native.unlocked_bosses = {"ali", "duke", "ramona"};  // marko's rule is met, his line not yet queued
  native.heard_dialogs = {"boss_unlock_ali", "boss_unlock_duke", "boss_unlock_ramona"};
  CHECK(!race::imported_campaign_needs_seed(native));
  race::CampaignFlow flow;
  race::CampaignSaveState st;
  st.campaign_stage = native.campaign_stage, st.heard_dialogs = native.heard_dialogs;
  st.unlocked_bosses = native.unlocked_bosses;
  flow.load_from(st);
  flow.on_wristband_points_changed(native.wristband_points, native.collected_wristband);
  CHECK(pending_dialog(flow) > 0);
  // Seeding it (the old owned_garage gate) would have swallowed that line.
  drive::DriveSave seeded = native;
  race::seed_imported_campaign(seeded, 0);
  CHECK(std::count(seeded.heard_dialogs.begin(), seeded.heard_dialogs.end(), "boss_unlock_marko") == 1);
  // A fresh engine career (no prize car yet) is not an import either.
  CHECK(!race::imported_campaign_needs_seed(drive::DriveSave{}));
}

// ---- 10. VIDEO screen settings (brightness, subtitles, video_seen / Main/Flags 0x40000) round-trip.
void test_video_settings_round_trip() {
  const auto dir = std::filesystem::temp_directory_path() / "shift_save_import_video_settings";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto save_path = dir / "save.txt";
  drive::DriveSave out;
  out.car = "VIP_Viper_13";
  out.video_seen = true;
  out.brightness = 75;
  out.has_subtitles = true;
  out.subtitles = true;
  std::string err;
  CHECK(drive::write_save(save_path, out, &err));
  drive::DriveSave back;
  CHECK(drive::read_save(save_path, back, &err));
  CHECK(back.video_seen);
  CHECK(back.brightness == 75);
  CHECK(back.has_subtitles && back.subtitles);

  // Profile import/export: Main/Gamma (snapped to 5%), Main/Subtitles, Main/Flags bit 0x40000.
  profile::Imported p;
  profile::set_field_double(p.body.fields, "Main/Gamma", 0.76, profile::Value::kFloat);
  profile::set_field_int(p.body.fields, "Main/Subtitles", 1, profile::Value::kU8);
  profile::set_field_int(p.body.fields, "Main/Flags", 0x40000, profile::Value::kU32);
  drive::DriveSave imported;
  profile::to_drive_save(p, [](int64_t) { return std::string{}; }, imported, nullptr);
  CHECK(imported.video_seen);
  CHECK(imported.brightness == 75);
  CHECK(imported.has_subtitles && imported.subtitles);

  imported.brightness = 40;
  imported.subtitles = false;
  profile::Imported exported;
  profile::from_drive_save(imported, garage::Garage{}, exported, &p);
  const auto* gf = profile::find(exported.body.fields, "Main/Gamma");
  const auto* sf = profile::find(exported.body.fields, "Main/Subtitles");
  const auto* ff = profile::find(exported.body.fields, "Main/Flags");
  CHECK(gf && std::fabs(gf->value.as_double() - 0.40) < 1e-4);
  CHECK(sf && sf->value.as_int() == 0);
  CHECK(ff && (uint64_t(ff->value.as_int()) & 0x40000ull) != 0);

  std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_sponsorship_codec();
  test_import_reads_saved_progress();
  test_career_stage();
  test_campaign_inference();
  test_driver_level();
  test_export_round_trip();
  test_rank_from_points();
  test_import_run_then_reload();
  test_engine_career_not_seeded();
  test_video_settings_round_trip();
  if (g_failures) {
    std::printf("save_import_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("save_import_tests: all passed\n");
  return 0;
}
