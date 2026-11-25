// Progression rules read from the game's data (PROG-02/07/08/10/12): the
// event bonus labels, the street hubs, the prize cars' upgrade class, the
// star drivers, the fast-travel cost and the barn finds. Synthetic checks
// always; --corpus reads the player's disc (exit 77 without
// SHIFT_GAME_ROOT, a failure when it is set and the data does not load).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include "freeroam/tunables.h"
#include "race/career.h"
#include "race/event.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

void test_event_bonus_meaning() {
  using K = race::EventBonus::Kind;
  using F = race::EventBonus::Form;
  race::EventBonus b;
  const std::pair<K, F> want[] = {
      {K::kAffinity, F::kFlat},    {K::kAffinity, F::kPercent},    {K::kAffinity, F::kRandom},
      {K::kDriverLevel, F::kFlat}, {K::kDriverLevel, F::kPercent}, {K::kDriverLevel, F::kRandom},
      {K::kCredits, F::kFlat},     {K::kCredits, F::kPercent},     {K::kCredits, F::kRandom},
      {K::kMystery, F::kRandom},
  };
  for (int t = 0; t < 10; ++t) {
    b.type_id = t;
    CHECK(b.kind() == want[t].first);
    CHECK(b.form() == want[t].second);
  }
}

void test_registry_and_fast_travel() {
  const char* xml = R"(<Registry>
  <Value id="Activities/BarnFinds/MaxIdleTime" type="float" value="120.000000"/>
  <Value id="Activities/BarnFinds/TimeBetweenTicks" type="float" value="60.000000"/>
  <Value id="Activities/Outposts/Discount_1_Missions" type="float" value="0.100000"/>
  <Value id="Activities/Outposts/TotalCost" type="int" value="2000"/>
  <Value id="Some/Name" type="string" value="abc"/>
</Registry>)";
  freeroam::Registry reg;
  std::string err;
  CHECK(freeroam::parse_registry(xml, reg, &err));
  CHECK(reg.values.size() == 4);  // the string value is skipped
  CHECK(reg.get("Activities/Outposts/TotalCost", 0) == 2000);
  CHECK(reg.get("Missing/Id", 7.5) == 7.5);
  const auto ft = freeroam::FastTravelTuning::from(reg);
  CHECK(ft.total_cost == 2000);
  CHECK(ft.discount[1] == 0.1 && ft.discount[2] == 0.5);  // unset ids keep the disc's values
  CHECK(freeroam::fast_travel_cost(ft, 0, 3) == 2000);
  CHECK(freeroam::fast_travel_cost(ft, 1, 3) == 1800);
  CHECK(freeroam::fast_travel_cost(ft, 3, 3) == 0);
  const auto bt = freeroam::BarnFindTuning::from(reg);
  CHECK(bt.max_idle_time_s == 120.0 && bt.time_between_ticks_s == 60.0);
  CHECK(bt.min_time_before_spawn_s == 15.0);
  freeroam::Registry empty;
  CHECK(!freeroam::parse_registry("<Registry/>", empty, &err));
}

}  // namespace

int corpus() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) {
    std::printf("progression_tests --corpus: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  const std::filesystem::path game(root);
  std::string err;

  // The career catalog: EventBonuses, the hubs and the prize cars.
  race::CareerCatalog cat;
  const bool loaded = race::load_career_catalog(game, cat, &err);
  CHECK(loaded);
  if (!loaded) std::printf("career catalog: %s\n", err.c_str());
  CHECK(cat.event_bonuses.size() == 46);
  if (const auto* b = cat.find_event_bonus(1)) {
    CHECK(b->kind() == race::EventBonus::Kind::kAffinity && b->form() == race::EventBonus::Form::kFlat);
    CHECK(b->amount == 500);
  } else {
    CHECK(!"EventBonuses row 1 missing");
  }
  if (const auto* b = cat.find_event_bonus(46)) CHECK(b->kind() == race::EventBonus::Kind::kMystery);
  int with_bonus = 0;
  for (const auto& ev : cat.events) {
    CHECK(ev.bonus_id == 1);
    with_bonus += ev.bonus_id == 1;
  }
  std::printf("events %zu (BonusId 1: %d), bonuses %zu\n", cat.events.size(), with_bonus, cat.event_bonuses.size());

  CHECK(cat.hubs.size() == 3);
  const int hub_pts[] = {310, 1700, 4500};
  for (size_t i = 0; i < cat.hubs.size() && i < 3; ++i) {
    CHECK(cat.hubs[i].unlock_points_req == hub_pts[i]);
    race::CareerRecord rec;
    rec.wristband_points = hub_pts[i] - 1;
    CHECK(!cat.is_hub_unlocked(cat.hubs[i], rec, 6));
    rec.wristband_points = hub_pts[i];
    CHECK(cat.is_hub_unlocked(cat.hubs[i], rec, -1));
  }

  // Rewards_EventPrizes.RewardCarUpgradeClass of the playable events.
  const std::map<int64_t, int> prize_class = {{79, 4}, {169, 8}, {59, 5}, {101, 3}};
  int prizes = 0;
  for (const auto& ev : cat.events) {
    if (!ev.prize_car_id) continue;
    ++prizes;
    CHECK(ev.prize_car_upgrade_class >= 3 && ev.prize_car_upgrade_class <= 8);
    CHECK(ev.prize_car_livery_level == 0 && ev.prize_car_tuning_level == 0);
    if (auto it = prize_class.find(ev.event_id); it != prize_class.end())
      CHECK(ev.prize_car_upgrade_class == it->second);
  }
  std::printf("prize events in the catalog: %d\n", prizes);
  CHECK(prizes > 0);

  // The star drivers: AIPlayers.Nemesis in festival event 4 (FR05): one star.
  race::EventData ev4;
  std::string e4;
  if (race::load_event(game, "FR05", race::Difficulty::kMed, ev4, &e4)) {
    int stars = 0;
    for (const auto& o : ev4.opponents) stars += o.nemesis;
    CHECK(stars == 1);
  } else {
    std::printf("event 4: %s\n", e4.c_str());
    CHECK(!"event 4 did not load");
  }

  // GlobalRegistry.xml: the fast-travel price and the barn-find tuning.
  freeroam::Registry reg;
  std::string rerr;
  const bool have_reg = freeroam::load_registry(game, reg, &rerr);
  CHECK(have_reg);
  if (!have_reg) std::printf("registry: %s\n", rerr.c_str());
  const auto ft = freeroam::FastTravelTuning::from(reg);
  CHECK(reg.has("Activities/Outposts/TotalCost"));
  CHECK(ft.total_cost == 10000);
  CHECK(ft.discount[0] == 0.0 && ft.discount[1] == 0.25 && ft.discount[2] == 0.5 && ft.discount[3] == 1.0);
  CHECK(freeroam::fast_travel_cost(ft, 0, 3) == 10000 && freeroam::fast_travel_cost(ft, 1, 3) == 7500 &&
        freeroam::fast_travel_cost(ft, 2, 3) == 5000 && freeroam::fast_travel_cost(ft, 3, 3) == 0);
  const auto bt = freeroam::BarnFindTuning::from(reg);
  CHECK(reg.has("Activities/BarnFinds/TimeBetweenTicks"));
  CHECK(bt.max_idle_time_s == 600.0 && bt.time_between_ticks_s == 300.0 && bt.min_time_before_spawn_s == 15.0);
  CHECK(bt.time_prompt_on_screen_s == 8.0);
  CHECK(std::abs(bt.min_distance_before_next_m - 48280.32) < 0.01);
  CHECK(std::abs(bt.min_speed_before_spawn_ms - 8.9408) < 1e-4);
  std::printf("registry: %zu values; fast travel %lld CR, discounts %.2f/%.2f/%.2f/%.2f\n", reg.values.size(),
              (long long)ft.total_cost, ft.discount[0], ft.discount[1], ft.discount[2], ft.discount[3]);

  std::printf("progression_tests --corpus: %d failures\n", failures);
  return failures ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--corpus") return corpus();
  test_event_bonus_meaning();
  test_registry_and_fast_travel();
  std::printf("progression_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
