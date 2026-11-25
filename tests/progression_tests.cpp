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

#include "cars/physics.h"
#include "cars/sources.h"
#include "cars/strings.h"
#include "fh1data/stringtables.h"
#include "freeroam/collectibles.h"
#include "freeroam/skills.h"
#include "freeroam/tunables.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "race/campaign_flow.h"
#include "race/career.h"
#include "race/showcase.h"
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

  // Every prize car fits its RewardCarUpgradeClass through the garage's
  // auto upgrade (what the award does), from its stock class.
  {
    cars::Install install;
    std::string gerr;
    cars::PartsDb db;
    cars::PhysicsSettings phys;
    garage::PiConfig pi_cfg;
    std::vector<garage::ClassBand> bands;
    const bool ok = cars::discover_install(game, "", "", install, &gerr) && db.open(install, &gerr) &&
                    cars::load_physics_settings(install.media / "physics.zip", phys, &gerr) &&
                    garage::load_pi_config(install.media / "physics.zip", pi_cfg, &gerr) &&
                    garage::load_class_bands(game, bands, &gerr);
    CHECK(ok);
    if (!ok) std::printf("garage data: %s\n", gerr.c_str());
    int upgraded = 0;
    for (const auto& ev : cat.events) {
      if (!ok || !ev.prize_car_id || ev.prize_car_upgrade_class < 0) continue;
      garage::Garage g;
      std::map<std::string, int64_t> stock;
      db.stock_slots(ev.prize_car_id, stock, &gerr);
      garage::PiResult pi0;
      CHECK(garage::compute_pi_for_car(db, ev.prize_car_id, {}, phys, pi_cfg, bands, pi0, &gerr));
      int64_t gid = 0;
      CHECK(garage::award_prize_car(g, ev.prize_car_id, pi0.raw_pi, pi0.class_id, 0, stock, &gid));
      if (pi0.class_id >= ev.prize_car_upgrade_class) {
        ++upgraded;  // already there from stock
        continue;
      }
      const auto plan =
          garage::plan_auto_upgrade(g, db, gid, ev.prize_car_upgrade_class, 0, phys, pi_cfg, bands);
      int64_t grant = plan.total_cost;
      std::string why;
      const bool applied =
          plan.ok && garage::apply_auto_upgrade(g, db, gid, plan, 0, grant, phys, pi_cfg, bands, &why);
      const garage::Car* car = g.find(gid);
      const bool in_class = applied && car && car->class_id == ev.prize_car_upgrade_class;
      if (!in_class)
        std::printf("prize %lld (event %lld): class %d -> %d failed: %s\n", (long long)ev.prize_car_id,
                    (long long)ev.event_id, pi0.class_id, ev.prize_car_upgrade_class,
                    (why.empty() ? plan.message : why).c_str());
      CHECK(in_class);
      CHECK(grant == 0);
      upgraded += in_class;
    }
    std::printf("prize cars at their upgrade class: %d / %d\n", upgraded, prizes);
    CHECK(upgraded == prizes);
  }

  // The barn finds: 9, their spawn odds and restoration distances, and the
  // controller run on the disc's data (a scripted random source).
  {
    freeroam::Collectibles col;
    std::string cerr;
    const bool ok = freeroam::load_collectibles(game, col, &cerr);
    CHECK(ok);
    if (!ok) std::printf("collectibles: %s\n", cerr.c_str());
    CHECK(col.barns.size() == 9);
    int spawn_rows = 0;
    for (const auto& b : col.barns) {
      spawn_rows += b.spawn_weighting == 100 && b.spawn_max_probability == 1 && b.spawn_max_distance_m > 0;
      CHECK(std::abs(freeroam::barn_restore_distance(b, 0) - 16093.44) < 0.01);
      CHECK(std::abs(freeroam::barn_restore_distance(b, 1e6) - 8046.72) < 0.01);
    }
    CHECK(spawn_rows == 9);
    // Barn 01 (1-in-10 at 0 mi, 1-in-1 at 200 mi): certain past 200 mi of odometer.
    freeroam::Progress prog;
    prog.total_distance = 200 * 1609.344 + 1;
    freeroam::FreeRoam fr(col, prog);
    fr.set_barn_tuning(bt.time_between_ticks_s, bt.max_idle_time_s, bt.min_distance_before_next_m,
                       bt.min_speed_before_spawn_ms, bt.min_time_before_spawn_s);
    fr.set_random([] { return 0.0; });  // the pick lands on barn 01
    freeroam::CarState car;
    // The tick lands at the end of the first step; the 1 s step after it
    // starts the pending timer, and the spawn time finishes it.
    car.x = 0, car.z = 0, car.speed = 20, car.dt = bt.time_between_ticks_s - 1;
    auto ev = fr.step(car);
    car.dt = 1;
    ev = fr.step(car);
    CHECK(prog.barns[0].status == freeroam::kBarnPending);
    car.dt = bt.min_time_before_spawn_s;
    ev = fr.step(car);
    CHECK(ev.size() == 1 && ev[0].kind == freeroam::Event::kRumourStarted && ev[0].index == 0);
    int rumours = 0;
    for (const auto& b : prog.barns) rumours += b.status == freeroam::kBarnRumour;
    std::printf("barn finds: %zu, spawn rows %d, rumours after one tick past 200 mi: %d\n", col.barns.size(),
                spawn_rows, rumours);
  }

  // The campaign tables from the disc, with its English strings, equal the
  // built-in copy field for field (the copy is what runs without a disc).
  {
    cars::Install install;
    fh1::StringTables strings;
    std::string serr;
    const bool sok = cars::discover_install(game, "", "", install, &serr) &&
                     cars::load_string_tables(install, strings, &serr);
    CHECK(sok);
    if (!sok) std::printf("strings: %s\n", serr.c_str());
    race::reset_campaign_tables();
    const auto bands0 = race::wristband_milestones();
    const auto bosses0 = race::boss_rivals();
    const auto shows0 = race::showcase_milestones();
    race::CampaignTablesLoad tl;
    const bool tok = race::load_campaign_tables(
        game, {}, nullptr,
        [&](std::string_view ref) {
          auto t = strings.resolve(ref);
          return t ? *t : std::string();
        },
        &tl, &serr);
    CHECK(tok);
    if (!tok) std::printf("campaign tables: %s\n", serr.c_str());
    CHECK(tl.wristbands == 7 && tl.bosses == 7 && tl.showcases == 10 && tl.captions_missing == 0);
    const auto& bands = race::wristband_milestones();
    const auto& bosses = race::boss_rivals();
    const auto& shows = race::showcase_milestones();
    int diffs = 0;
    auto same = [&](bool eq, const std::string& what) {
      if (!eq) {
        ++diffs;
        std::printf("campaign table differs from the disc: %s\n", what.c_str());
      }
    };
    CHECK(bands.size() == bands0.size() && bosses.size() == bosses0.size() && shows.size() == shows0.size());
    for (size_t i = 0; i < bands.size() && i < bands0.size(); ++i) {
      const auto &a = bands[i], &b = bands0[i];
      same(a.level == b.level && a.key == b.key && a.display_name == b.display_name && a.colour_key == b.colour_key &&
               a.xp_threshold == b.xp_threshold && a.nemesis_bonus == b.nemesis_bonus &&
               a.vo_unlock_trigger == b.vo_unlock_trigger && a.vo_new_trigger == b.vo_new_trigger &&
               a.radio_event == b.radio_event && a.announcement == b.announcement &&
               a.unlocked_events_hint == b.unlocked_events_hint,
           "wristband " + a.display_name + " / " + b.display_name);
    }
    for (size_t i = 0; i < bosses.size() && i < bosses0.size(); ++i) {
      const auto &a = bosses[i], &b = bosses0[i];
      same(a.index == b.index && a.wristband_level == b.wristband_level && a.event_id == b.event_id &&
               a.horizon_id == b.horizon_id && a.key == b.key && a.character == b.character &&
               a.rival_name == b.rival_name && a.rival_title == b.rival_title && a.car_id == b.car_id &&
               a.car_media == b.car_media && a.car_display == b.car_display && a.unlock_points == b.unlock_points &&
               a.cash_prize == b.cash_prize && a.vo_unlocked == b.vo_unlocked && a.vo_pre_race == b.vo_pre_race &&
               a.vo_post_win == b.vo_post_win && a.challenge_line == b.challenge_line &&
               a.victory_line == b.victory_line && a.is_finale_champion == b.is_finale_champion,
           "star showdown " + a.horizon_id + " (" + a.rival_name + ", " + a.car_display + ")");
      // The pink slip is the event's prize car in the catalog too.
      if (const auto* ev = cat.find_event(a.horizon_id)) CHECK(ev->prize_car_id == a.car_id);
    }
    for (size_t i = 0; i < shows.size() && i < shows0.size(); ++i) {
      const auto &a = shows[i], &b = shows0[i];
      same(a.index == b.index && a.event_id == b.event_id && a.horizon_id == b.horizon_id &&
               a.display_name == b.display_name && a.opponent_label == b.opponent_label &&
               a.popularity_rank_req == b.popularity_rank_req && a.loaner_car_id == b.loaner_car_id &&
               a.loaner_car_media == b.loaner_car_media && a.loaner_car_display == b.loaner_car_display &&
               a.cash_prize == b.cash_prize && a.airborne == b.airborne && a.intro_line == b.intro_line &&
               a.victory_line == b.victory_line,
           "showcase " + a.horizon_id + " (" + a.display_name + ", " + a.opponent_label + ", " +
               a.loaner_car_display + ")");
    }
    CHECK(diffs == 0);
    std::printf("campaign tables: %d wristbands, %d star showdowns, %d showcases, %d captions missing, %d diffs\n",
                tl.wristbands, tl.bosses, tl.showcases, tl.captions_missing, diffs);

    // The campaign's shipped captions resolve on the disc to the English
    // fallbacks the flow carries.
    race::set_campaign_caption_resolver([&](std::string_view ref) {
      auto t = strings.resolve(ref);
      return t ? *t : std::string();
    });
    {
      race::CampaignFlow flow;
      CHECK(flow.next_objective(1500, 2, 250).title == "COLLECT PINK WRISTBAND");
      flow.on_barn_find(true, "barnfind_01", "1964 Aston Martin DB5 Vantage");
      CHECK(flow.cinematic().detail_line == "1964 Aston Martin DB5 Vantage IS BEING RESTORED");
      flow.on_barn_restored("barnfind_01", "1964 Aston Martin DB5 Vantage");
      CHECK(flow.cinematic().headline == "BARN FIND READY - 1964 Aston Martin DB5 Vantage");
      CHECK(flow.cinematic().detail_line == "Head to the garage to collect it!");
    }
    race::set_campaign_caption_resolver({});

    // The Showcase opponents: airborne_challenges.xml's pilot and vehicle
    // strings, which the built-in AirChallenges text matches.
    std::vector<race::ShowcaseSpec> air;
    CHECK(race::load_airborne_challenges(game, air, &serr));
    int named = 0;
    for (const auto& sp : air) {
      auto ref = [&](const std::string& ids) {
        return "_&" + std::to_string((uint32_t(fh1::string_hash("AirChallenges")) << 16) | fh1::string_hash(ids));
      };
      const auto pilot = strings.resolve(ref(sp.pilot_name_ids));
      const auto veh = strings.resolve(ref(sp.vehicle_name_ids));
      named += pilot && veh && *pilot == sp.display_name && *veh == sp.vehicle_name && !sp.display_name.empty();
    }
    CHECK(air.size() == 7 && named == 7);
    std::printf("showcase opponents named from AirChallenges: %d / %zu\n", named, air.size());
    race::reset_campaign_tables();
  }

  // The chain multiplier on the disc's skills: five single skills show X1.4
  // (the oracle frame's "583 X1.4"), and 41 cap it at X5.0.
  {
    freeroam::SkillsData sd;
    std::string kerr;
    const bool kok = freeroam::load_skills_data(game, sd, &kerr);
    CHECK(kok);
    if (!kok) std::printf("skills: %s\n", kerr.c_str());
    freeroam::SkillTracker t(sd, 0.0, 0);
    for (int i = 1; i <= 5; ++i) t.award_named(i % 2 ? "Drift" : "NearMiss", 1, 1.0 + i * 0.01);
    const float m5 = t.chain_multiplier();
    for (int i = 6; i <= 45; ++i) t.award_named(i % 2 ? "Drift" : "NearMiss", 1, 1.0 + i * 0.01);
    const float m45 = t.chain_multiplier();
    CHECK(std::fabs(m5 - 1.4f) < 1e-5f);
    CHECK(std::fabs(m45 - 5.0f) < 1e-5f);
    CHECK(freeroam::banked_chain_points(583, m5) == 816);
    std::printf("chain multiplier on the disc's skills: X%.1f at 5, X%.1f at 45\n", m5, m45);
  }

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
