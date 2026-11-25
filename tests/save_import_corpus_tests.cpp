// Importing the two reference saves resumes where they were (PROG-13,
// PROG-14, PROG-15), against the disc's data: popularity rank 115 (`user`)
// and 28 (`user-modded`) from SponsorshipState on the Fame.xml ladder; the
// Fame tiers and every sponsor's tiers agree with SponsorshipChallenges.xml;
// the showcase unlocks the engine derives at that rank are exactly the ones
// the save stores; MaxSkillsCarOrdinal and the saved level are kept; the
// collected wristband, bosses and finale carry; nothing is queued again; an
// export with the save as template round-trips the same fields.
//
// Needs SHIFT_GAME_ROOT (the disc) and SHIFT_STATE_ROOT (the launcher state
// root); the plaintext exports are SHIFT_PROFILE_EXPORTS, else
// `<SHIFT_STATE_ROOT>/../profile` (the out/ tree). Exit 77 without them.
// Never writes to the reference trees: the export goes to a temp folder.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "drive/save.h"
#include "freeroam/skills.h"
#include "garage/garage.h"
#include "profile/drive_save.h"
#include "profile/profile.h"
#include "race/campaign_flow.h"
#include "race/career.h"

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

struct Expect {
  const char* tree;
  int rank, fame_tiers, wristband;
  int64_t max_skills_car;
  bool finale;
  size_t bosses;
};

const profile::Record* find_record(const profile::Imported& p, const char* name) {
  for (const auto& blk : p.body.blocks)
    for (const auto& r : blk.records)
      if (r.name == name) return &r;
  return nullptr;
}

void check_tree(const fs::path& folder, const Expect& e, const freeroam::SkillsData& sk,
                const race::CareerCatalog& cat) {
  std::printf("-- %s (%s)\n", e.tree, folder.string().c_str());
  profile::Imported p;
  std::string err;
  if (!profile::import_profile(folder, p, &err)) {
    std::printf("FAIL %s: %s\n", e.tree, err.c_str());
    ++g_failures;
    return;
  }
  drive::DriveSave ds;
  profile::to_drive_save(p, [](int64_t) { return std::string(); }, ds, nullptr);

  // PROG-13: popularity and the sponsors.
  CHECK(p.has_sponsorship);
  const int rank = sk.rank_for_points(ds.popularity_points);
  std::printf("   popularity %.0f -> rank %d (expected %d)\n", ds.popularity_points, rank, e.rank);
  CHECK(rank == e.rank);
  int fame_due = 0;
  for (const auto& t : sk.fame_tiers) fame_due += rank <= t.rank_required;
  std::printf("   fame tiers claimed %d, due at that rank %d\n", ds.fame_rewards_claimed, fame_due);
  CHECK(ds.fame_rewards_claimed == e.fame_tiers && ds.fame_rewards_claimed == fame_due);
  int sponsors_ok = 0;
  for (const auto& sp : ds.sponsors) {
    for (const auto& sc : sk.sponsor_challenges) {
      if (sc.id != sp.id) continue;
      int due = 0;
      for (const auto& t : sc.tiers) due += sp.count >= t.number;
      CHECK(due == sp.claimed);
      sponsors_ok += due == sp.claimed;
    }
  }
  std::printf("   %d of %zu sponsors' tiers match their counts\n", sponsors_ok, ds.sponsors.size());
  CHECK(ds.sponsors.size() == 14 && sponsors_ok == 14);

  // PROG-14: the real field names; the level kept as saved.
  CHECK(ds.max_skills_car == e.max_skills_car && ds.max_skills_score > 0);
  CHECK(ds.level == profile::engine_driver_level_from_profile(p.level));
  CHECK(ds.driver_xp == 0);

  // PROG-15: the wristband, the campaign mirror, the showcases.
  CHECK(ds.collected_wristband == e.wristband && ds.intro_completed);
  race::seed_imported_campaign(ds, rank);
  race::CareerRecord rec;
  rec.wristband_points = ds.wristband_points;
  for (const auto& [id, place] : ds.event_results) rec.event_places[id] = place;
  const std::set<int64_t> stored(p.event_unlocks.begin(), p.event_unlocks.end());
  std::set<std::string> showcase_keys(ds.unlocked_showcases.begin(), ds.unlocked_showcases.end());
  int showcases = 0, showcase_match = 0;
  for (const auto& ev : cat.events) {
    if (ev.style != 3 || ev.popularity_rank_req <= 0 || ev.popularity_rank_req > 250) continue;
    ++showcases;
    const bool engine = cat.is_event_unlocked(ev, rec, ds.collected_wristband, rank);
    const bool saved = stored.count(ev.event_id) > 0;
    const bool mirror = showcase_keys.count(ev.horizon_id) > 0;
    if (engine != saved || mirror != saved)
      std::printf("   showcase %s: engine %d, save %d, mirror %d\n", ev.horizon_id.c_str(), engine, saved, mirror);
    CHECK(engine == saved && mirror == saved);
    showcase_match += engine == saved && mirror == saved;
  }
  std::printf("   showcases: %d of %d match the save's CareerEventUnlocks\n", showcase_match, showcases);
  CHECK(showcases == 10);
  CHECK(ds.finale_complete == e.finale);
  CHECK(ds.unlocked_bosses.size() == e.bosses);
  // Every Star Showdown the save raced or stores is unlocked.
  for (const auto& b : race::boss_rivals()) {
    const bool raced = rec.event_places.count(b.event_id) > 0;
    const bool in_save = stored.count(b.event_id) > 0;
    if (raced || in_save) CHECK(std::count(ds.unlocked_bosses.begin(), ds.unlocked_bosses.end(), b.key) == 1);
  }
  std::printf("   wristband %d collected, %zu bosses, finale %s\n", ds.collected_wristband, ds.unlocked_bosses.size(),
              ds.finale_complete ? "won" : "open");
  {
    race::CampaignFlow flow;
    drive::DriveSave copy = ds;
    race::CampaignSaveState st;
    st.campaign_stage = copy.campaign_stage, st.completed_cutscenes = copy.completed_cutscenes;
    st.heard_dialogs = copy.heard_dialogs, st.unlocked_bosses = copy.unlocked_bosses;
    st.unlocked_showcases = copy.unlocked_showcases, st.finale_complete = copy.finale_complete;
    flow.load_from(st);
    flow.on_wristband_points_changed(ds.wristband_points, ds.collected_wristband);
    flow.on_popularity_rank_changed(rank + 1, rank);
    const size_t pending = flow.dialogs().queued_count() + (flow.dialogs().active() ? 1 : 0);
    std::printf("   dialogue queued after load: %zu\n", pending);
    CHECK(pending == 0);
    CHECK(flow.finale_complete() == e.finale);
  }

  // Export round trip (the save as template; output in a temp folder).
  const fs::path out = fs::temp_directory_path() / "shift_save_import_corpus" / e.tree;
  std::error_code ec;
  fs::remove_all(out, ec);
  profile::ExportOptions opts;
  opts.template_folder = folder;
  const garage::Garage g = profile::to_garage(p);
  CHECK(profile::export_forza_profile(ds, g, out, &err, opts));
  profile::Imported back;
  CHECK(profile::import_profile(out, back, &err));
  std::printf("   export: Main %zu fields (was %zu)\n", back.main_fields, p.main_fields);
  CHECK(back.main_fields == p.main_fields);
  CHECK(!profile::find(back.body.fields, "Main/Popularity") && !profile::find(back.body.fields, "Main/DriverXP") &&
        !profile::find(back.body.fields, "Main/MaxSkillsCar"));
  const profile::Record* a = find_record(p, "SponsorshipState");
  const profile::Record* b = find_record(back, "SponsorshipState");
  CHECK(a && b && a->payload == b->payload);
  CHECK(back.popularity == p.popularity && back.fame_tiers_claimed == p.fame_tiers_claimed);
  CHECK(back.level == p.level && back.last_rewarded_level == p.last_rewarded_level);
  CHECK(back.wristband_level == p.wristband_level && back.xp == p.xp);
  CHECK(back.max_skills_score == p.max_skills_score && back.max_skills_car == p.max_skills_car);
  CHECK(back.event_results.size() == p.event_results.size() && back.event_unlocks.size() == p.event_unlocks.size());
  drive::DriveSave ds2;
  profile::to_drive_save(back, [](int64_t) { return std::string(); }, ds2, nullptr);
  race::seed_imported_campaign(ds2, sk.rank_for_points(ds2.popularity_points));
  CHECK(ds2.popularity_points == ds.popularity_points && ds2.collected_wristband == ds.collected_wristband);
  CHECK(ds2.unlocked_showcases == ds.unlocked_showcases && ds2.unlocked_bosses == ds.unlocked_bosses);
  CHECK(ds2.finale_complete == ds.finale_complete && ds2.level == ds.level);
  fs::remove_all(out, ec);
}

}  // namespace

int main() {
  const char* game = std::getenv("SHIFT_GAME_ROOT");
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  if (!game || !*game || !state || !*state) {
    std::printf("save_import_corpus_tests: SHIFT_GAME_ROOT / SHIFT_STATE_ROOT unset (skipped)\n");
    return 77;
  }
  fs::path exports;
  if (const char* e = std::getenv("SHIFT_PROFILE_EXPORTS"); e && *e) exports = e;
  else exports = fs::path(state).parent_path() / "profile";
  if (!fs::exists(exports / "user" / "ForzaProfile.body") || !fs::exists(exports / "user-modded" / "ForzaProfile.body")) {
    std::printf("save_import_corpus_tests: no reference exports under %s (skipped)\n", exports.string().c_str());
    return 77;
  }
  std::string err;
  freeroam::SkillsData sk;
  if (!freeroam::load_skills_data(game, sk, &err)) {
    std::printf("FAIL skills data: %s\n", err.c_str());
    return 1;
  }
  race::CareerCatalog cat;
  if (!race::load_career_catalog(game, state, nullptr, cat, &err)) {
    std::printf("FAIL career catalog: %s\n", err.c_str());
    return 1;
  }
  std::printf("save_import_corpus_tests: game %s, state %s, exports %s\n", game, state, exports.string().c_str());
  check_tree(exports / "user", {"user", 115, 5, 4, 633, false, 4}, sk, cat);
  check_tree(exports / "user-modded", {"user-modded", 28, 8, 6, 1392, true, 7}, sk, cat);
  if (g_failures) {
    std::printf("save_import_corpus_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("save_import_corpus_tests: all passed\n");
  return 0;
}
