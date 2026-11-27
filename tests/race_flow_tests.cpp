// The race-flow model (apps/carview/race_flow.h): the original's screen
// orders around a race, per race kind, and the timed advances, plus WP-D
// race ticket string-table verification when SHIFT_GAME_ROOT is set.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cars/sources.h"
#include "cars/strings.h"
#include "fh1data/stringtables.h"
#include "game_hud.h"
#include "race/career.h"
#include "race/event.h"
#include "race/session.h"
#include "race_flow.h"
#include "race_screens.h"

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

using namespace carview::race_flow;
using S = Screen;

namespace {

bool has(const std::vector<S>& v, S s) {
  for (S x : v)
    if (x == s) return true;
  return false;
}

void pre_race() {
  PreFacts f;
  CHECK((pre_steps(f) == std::vector<S>{S::kTitleCard, S::kGridMenu, S::kCountdown}));
  // The rival callout comes after START, before the count.
  f.nemesis_in_event = true;
  CHECK((pre_steps(f) == std::vector<S>{S::kTitleCard, S::kGridMenu, S::kRivalIntro, S::kCountdown}));
  // Restart: the menu only (RACE-27), never the callout.
  f.restart = true;
  CHECK((pre_steps(f) == std::vector<S>{S::kGridMenu, S::kCountdown}));
  // Rivals: straight to the menu, no callout.
  PreFacts r{Kind::kRivals, false, true};
  CHECK((pre_steps(r) == std::vector<S>{S::kGridMenu, S::kCountdown}));
  // Street and nemesis flows: no title screen, no callout.
  PreFacts st{Kind::kStreet, false, true};
  CHECK((pre_steps(st) == std::vector<S>{S::kGridMenu, S::kCountdown}));
  PreFacts nm{Kind::kNemesis, false, true};
  CHECK((pre_steps(nm) == std::vector<S>{S::kGridMenu, S::kCountdown}));
  // Showcase and headline keep the title.
  CHECK(pre_steps(PreFacts{Kind::kShowcase}).front() == S::kTitleCard);
  CHECK(pre_steps(PreFacts{Kind::kHeadline}).front() == S::kTitleCard);
}

void post_race() {
  PostFacts f;
  f.credits = 4200;
  // The original order: banner, cash, points, black-out, load.
  CHECK((post_steps(f) == std::vector<S>{S::kFinishBanner, S::kCashReward, S::kPoints, S::kBlackOut, S::kDone}));
  // No cash earned: no cash screen. No career points: no points screen.
  PostFacts z = f;
  z.credits = 0;
  CHECK(!has(post_steps(z), S::kCashReward));
  z.career_xp_event = false;
  CHECK((post_steps(z) == std::vector<S>{S::kFinishBanner, S::kBlackOut, S::kDone}));
  // DNF: the banner, then out.
  PostFacts dnf = f;
  dnf.crossed_line = false;
  CHECK((post_steps(dnf) == std::vector<S>{S::kFinishBanner, S::kBlackOut, S::kDone}));
  // Quit: straight to the load.
  PostFacts q = f;
  q.quit = true;
  CHECK((post_steps(q) == std::vector<S>{S::kBlackOut, S::kDone}));
  // A reward car ends the flow before the event-car check.
  PostFacts rc = f;
  rc.reward_car = true;
  rc.event_car = true;
  rc.tombola = true;
  auto rcs = post_steps(rc);
  CHECK(has(rcs, S::kRewardCar));
  CHECK(!has(rcs, S::kTombola));
  CHECK(!has(rcs, S::kStayInCar));
  // An event car with no reward car: STAY IN CAR?
  PostFacts ec = f;
  ec.event_car = true;
  CHECK((post_steps(ec) ==
         std::vector<S>{S::kFinishBanner, S::kCashReward, S::kPoints, S::kStayInCar, S::kBlackOut, S::kDone}));
  // The tombola stands in for the reward car.
  PostFacts tb = f;
  tb.tombola = true;
  CHECK(has(post_steps(tb), S::kTombola));
  // The rival offer only online, never on a first-time career.
  CHECK(!has(post_steps(f), S::kRivalOffer));
  PostFacts on = f;
  on.online = true;
  CHECK(has(post_steps(on), S::kRivalOffer));
  on.first_time_career = true;
  CHECK(!has(post_steps(on), S::kRivalOffer));
  // The first-time career's heats: the cash, then out (no points page).
  PostFacts ftc = f;
  ftc.first_time_career = true;
  CHECK((post_steps(ftc) == std::vector<S>{S::kFinishBanner, S::kCashReward, S::kBlackOut, S::kDone}));
  // Showcase: cash then the car, no points. Headline: no points.
  PostFacts sc = f;
  sc.kind = Kind::kShowcase;
  sc.reward_car = true;
  CHECK((post_steps(sc) == std::vector<S>{S::kFinishBanner, S::kCashReward, S::kRewardCar, S::kBlackOut, S::kDone}));
  PostFacts hl = f;
  hl.kind = Kind::kHeadline;
  CHECK(!has(post_steps(hl), S::kPoints));
  // Head-to-head: a win shows the banner and its cash; a loss nothing.
  PostFacts hw = f;
  hw.kind = Kind::kHeadToHead;
  hw.won = true;
  CHECK((post_steps(hw) == std::vector<S>{S::kFinishBanner, S::kCashReward, S::kDone}));
  hw.won = false;
  CHECK((post_steps(hw) == std::vector<S>{S::kDone}));
  // Rally: banner, earnings, rally points.
  PostFacts ra = f;
  ra.kind = Kind::kRally;
  ra.event_car = true;
  CHECK((post_steps(ra) == std::vector<S>{S::kFinishBanner, S::kCashReward, S::kPoints, S::kBlackOut, S::kDone}));
  // The nemesis flow keeps its AI on screen.
  CHECK(hides_opponents(Kind::kFestival));
  CHECK(!hides_opponents(Kind::kNemesis));
  CHECK(kind_of_style(5) == Kind::kNemesis);
  CHECK(kind_of_style(2) == Kind::kStreet);
  CHECK(kind_of_style(1) == Kind::kFestival);
}

void entry() {
  CHECK((entry_steps(true) == std::vector<S>{S::kEventSummary, S::kEntryFade}));
  // An ineligible car leads to car select, not a toast.
  CHECK((entry_steps(false) == std::vector<S>{S::kEventSummary, S::kCarSelect, S::kEntryFade}));
}

void timed() {
  Flow fl;
  fl.start(pre_steps(PreFacts{}));
  CHECK(fl.screen() == S::kTitleCard);
  // The menu comes 1.8 s after the card, by itself.
  CHECK(!fl.update(1.0));
  CHECK(fl.screen() == S::kTitleCard);
  CHECK(fl.update(0.9));
  CHECK(fl.screen() == S::kGridMenu);
  CHECK(!fl.ready());  // still growing in
  fl.update(0.2);
  CHECK(fl.ready());
  CHECK(fl.next());
  CHECK(fl.screen() == S::kCountdown);
  CHECK(!fl.ready());
  CHECK(fl.next());
  CHECK(!fl.active());

  PostFacts f;
  f.credits = 100;
  fl.start(post_steps(f));
  CHECK(fl.screen() == S::kFinishBanner);
  CHECK(!fl.update(30.0));  // the banner waits for A
  CHECK(fl.next());
  CHECK(fl.screen() == S::kCashReward);
  CHECK(!fl.ready());  // the banner is still going out
  fl.update(timing::kBannerOut + timing::kCashLines);
  CHECK(fl.ready());
  // Cash auto-advances to the points.
  fl.update(timing::kCashAuto);
  CHECK(fl.screen() == S::kPoints);
  CHECK(fl.next());
  CHECK(fl.screen() == S::kBlackOut);
  fl.update(timing::kBlackFade + timing::kBlackHold + 0.01);
  CHECK(fl.screen() == S::kDone);
  CHECK(!fl.next());  // the end holds
  CHECK(fl.screen() == S::kDone);

  // QUIT RACE from the menu jumps to the end; car select is inserted.
  fl.start(entry_steps(true));
  fl.insert_next(S::kCarSelect);
  fl.insert_next(S::kCarSelect);
  CHECK((fl.steps() == std::vector<S>{S::kEventSummary, S::kCarSelect, S::kEntryFade}));
  CHECK(fl.skip_to(S::kEntryFade));
  CHECK(!fl.skip_to(S::kCarSelect));
  CHECK(fl.update(timing::kEntryFade));
  CHECK(!fl.active());

  // The rival callout runs on nemesis_show's UI trigger (0.8 s, 10 s), then
  // half a second; a head-to-head's intro on RandomEncounter_Intro (3.5 s).
  CHECK(timing::kRivalIntro > 11.29 && timing::kRivalIntro < 11.31);
  fl.start({S::kRivalIntro, S::kCountdown});
  CHECK(!fl.update(11.0));
  CHECK(fl.update(0.31));
  CHECK(fl.screen() == S::kCountdown);
  fl.start({S::kEncounterIntro});
  CHECK(!fl.update(3.4));
  CHECK(fl.update(0.11));
  CHECK(!fl.active());
  CHECK(screen_name(S::kEncounterIntro) == "encounter_intro");
}

void disc_ticket_strings() {
  const char* root_env = std::getenv("SHIFT_GAME_ROOT");
  if (!root_env || !*root_env) return;
  const std::filesystem::path game_root = root_env;
  const char* state_env = std::getenv("SHIFT_STATE_ROOT");
  const std::filesystem::path state_root = state_env ? state_env : "";

  cars::Install inst;
  std::string why;
  CHECK(cars::discover_install(game_root, state_root, {}, inst, &why));
  fh1::StringTables tables;
  CHECK(cars::load_string_tables(inst, tables, &why, "EN"));

  auto lookup_ref = [&](const carview::StringRef& r) -> std::string {
    const uint32_t hi = fh1::string_hash(r.table);
    const uint32_t lo = r.id.empty() ? uint32_t(r.raw) : fh1::string_hash(r.id);
    const std::string key = "_&" + std::to_string((hi << 16) | (lo & 0xFFFFu));
    auto res = tables.resolve(key);
    return res ? *res : std::string{};
  };

  // Rank 15: verify real disc CareerEventUI strings for Heats & Showcase tickets.
  const std::string heats_info = lookup_ref(carview::race_text::kHorizonHeatsInfo);
  const std::string heats_l1 = lookup_ref(carview::race_text::kHorizonHeatsReward1);
  const std::string heats_l2 = lookup_ref(carview::race_text::kHorizonHeatsReward2);
  const std::string showcase_info = lookup_ref(carview::race_text::kShowcaseInstructions);
  const std::string winner_takes = lookup_ref(carview::race_text::kWinnerTakes);
  std::printf("ticket strings: heats='%s' / '%s' / '%s', showcase='%s', winner='%s'\n", heats_info.c_str(),
              heats_l1.c_str(), heats_l2.c_str(), showcase_info.c_str(), winner_takes.c_str());
  CHECK(heats_info == "FINISH 3RD OR ABOVE TO WIN");
  CHECK(!heats_l1.empty());
  CHECK(!heats_l2.empty());
  CHECK(showcase_info == "WIN THE RACE TO KEEP THE CAR");
  CHECK(winner_takes == "WINNER TAKES");
}

void nemesis_grid_placement() {
  auto make_opponents = [](size_t star_pos) {
    std::vector<race::Opponent> ops;
    for (size_t i = 0; i < 7; ++i) {
      race::Opponent o;
      o.participant_id = int64_t(i + 1);
      o.car_id = int64_t(1000 + i);
      o.performance_index = 0.70 - 0.02 * double(i);
      o.nemesis = (i == star_pos);
      ops.push_back(o);
    }
    return ops;
  };

  // FR01: PlayerGridPosition=7 (slot 6), NemesisGridPosition=1 (slot 0).
  {
    auto ops = make_opponents(4);  // star originally sorted 5th by PI
    const int pslot = race::player_slot(7, 8);
    const auto slots = race::opponent_slots(pslot, 7);
    race::place_nemesis_on_grid(ops, slots, 1);
    CHECK(ops[0].nemesis && slots[0] == 0);
    // Remaining opponents stay in relative PI order.
    CHECK(ops[1].car_id == 1000 && ops[2].car_id == 1001 && ops[3].car_id == 1002 && ops[4].car_id == 1003);
  }

  // FR06 / FR22: PlayerGridPosition=7 (slot 6), NemesisGridPosition=3 (slot 2).
  {
    auto ops = make_opponents(0);  // star originally on pole by PI
    const int pslot = race::player_slot(7, 8);
    const auto slots = race::opponent_slots(pslot, 7);
    race::place_nemesis_on_grid(ops, slots, 3);
    CHECK(ops[2].nemesis && slots[2] == 2);
    CHECK(ops[0].car_id == 1001 && ops[1].car_id == 1002 && ops[3].car_id == 1003);
  }

  // HEADLINE_EVENT: NemesisGridPosition=0 leaves PI order untouched.
  {
    auto ops = make_opponents(3);
    const int pslot = race::player_slot(0, 8);
    const auto slots = race::opponent_slots(pslot, 7);
    race::place_nemesis_on_grid(ops, slots, 0);
    CHECK(ops[3].nemesis);
  }

  // Clamping: out-of-range NemesisGridPosition clamps to last available slot.
  {
    auto ops = make_opponents(0);
    const int pslot = race::player_slot(7, 8);
    const auto slots = race::opponent_slots(pslot, 7);
    race::place_nemesis_on_grid(ops, slots, 99);
    CHECK(ops[6].nemesis && slots[6] == 7);
  }
}

void missed_checkpoint_hairpin_and_fade() {
  carview::MissedCheckpointTracker tr;
  // Approach gate at progress 600 on incoming leg of a tight hairpin.
  CHECK(!tr.step(1.0 / 120.0, true, false, 2, 550.0, 600.0, -50.0, 50.0, 18.0, 2.0));
  CHECK(!tr.active);
  // Spline snaps +95 m onto outgoing leg in one tick while car is still physically
  // behind the gate plane (gate_plane_side < 0): must NOT false-trigger!
  CHECK(!tr.step(1.0 / 120.0, true, false, 2, 645.0, 600.0, -45.0, 48.0, 18.0, 3.0));
  CHECK(!tr.active);
  // Even if the car keeps creeping on the incoming leg while snapped, stay clear.
  CHECK(!tr.step(1.0 / 120.0, true, false, 2, 646.0, 600.0, -44.0, 47.0, 18.0, 3.0));
  CHECK(!tr.active);

  // Genuine monotonic bypass past gate plane outside gate reach: triggers 4.0 s countdown.
  tr.reset();
  CHECK(!tr.step(0.1, true, false, 2, 595.0, 600.0, -5.0, 22.0, 18.0, 5.0));
  CHECK(!tr.step(0.1, true, false, 2, 610.0, 600.0, 10.0, 24.0, 18.0, 6.0));
  CHECK(!tr.step(0.1, true, false, 2, 618.0, 600.0, 18.0, 26.0, 18.0, 6.0));
  CHECK(tr.active);
  // Returning cleanly before the gate cancels countdown.
  CHECK(!tr.step(0.1, true, false, 2, 595.0, 600.0, -5.0, 10.0, 18.0, 2.0));
  CHECK(!tr.active);

  // Drive past gate again and let countdown expire -> returns true to trigger fade reset.
  CHECK(!tr.step(0.1, true, false, 2, 618.0, 600.0, 18.0, 26.0, 18.0, 6.0));
  CHECK(tr.active);
  bool fired = false;
  for (int i = 0; i < 45; ++i) {
    if (tr.step(0.1, true, false, 2, 630.0, 600.0, 30.0, 35.0, 18.0, 6.0)) {
      fired = true;
      break;
    }
  }
  CHECK(fired);

  // MissedCheckpointFade: smooth fade-out -> reposition once at full black -> fade-in -> idle.
  carview::MissedCheckpointFade fade;
  CHECK(!fade.active() && fade.alpha() == 0.0f);
  fade.trigger(588.0, 0.30, 0.30);
  CHECK(fade.active());
  int resets = 0;
  float peak_alpha = 0.0f;
  for (int i = 0; i < 100; ++i) {
    if (fade.step(0.01)) {
      ++resets;
      CHECK(fade.alpha() >= 0.99f);
    }
    if (fade.alpha() > peak_alpha) peak_alpha = fade.alpha();
  }
  CHECK(resets == 1);
  CHECK(peak_alpha >= 0.99f);
  CHECK(!fade.active() && fade.alpha() == 0.0f);
}

void career_restriction_engine_placement_and_cylinders() {
  // Mask with Make (bit 19, 2B), EnginePlacement (bit 29, 1B), Cylinders (bit 31, 1B), Flag34 (bit 34, 0B).
  // Byte 0: 0x00 (bits 0..7)
  // Byte 1: 0x00 (bits 8..15)
  // Byte 2: 0x08 (bit 19)
  // Byte 3: 0xA0 (bit 29 = 0x20, bit 31 = 0x80)
  // Byte 4: 0x04 (bit 34)
  // Payloads in ascending bit order: Make=0x000C (12), EnginePlacement=0x02 (Mid), Cylinders=0x08 (8).
  shift::data::EventRestrictions res;
  race::CareerRestrictionExtras extras;
  CHECK(race::decode_career_restriction_bucket(res, 0, "000008A004000C0208", &extras));
  CHECK(res.buckets[0].make.has_value() && *res.buckets[0].make == 12u);
  CHECK(res.buckets[0].flag34);
  CHECK(extras.engine_placement == 2);
  CHECK(extras.cylinder_id == 8);

  // Unknown even bit 28 (0x10 in byte 3) is still rejected.
  shift::data::EventRestrictions bad;
  CHECK(!race::decode_career_restriction_bucket(bad, 0, "000000100002", nullptr));

  // Verify CareerCatalog::check_eligibility enforces both required_engine_placement and required_cylinders.
  race::CareerCatalog cat;
  cat.engine_placement_names = {{1, "Front"}, {2, "Mid"}, {3, "Rear"}};
  race::CarSpec front_v8;
  front_v8.car_id = 1;
  front_v8.props.make = 12;
  front_v8.engine_placement = 1;
  front_v8.cylinder_count = 8;
  cat.cars[1] = front_v8;

  race::CarSpec mid_i4 = front_v8;
  mid_i4.car_id = 2;
  mid_i4.engine_placement = 2;
  mid_i4.cylinder_count = 4;
  cat.cars[2] = mid_i4;

  race::CarSpec mid_v8 = front_v8;
  mid_v8.car_id = 3;
  mid_v8.engine_placement = 2;
  mid_v8.cylinder_count = 8;
  cat.cars[3] = mid_v8;

  race::CareerEvent ev;
  ev.restrictions = res;
  ev.required_engine_placement = extras.engine_placement;
  ev.required_cylinders = extras.cylinder_id;
  const std::vector<std::string> classes = {"E", "D", "C", "B", "A", "S", "R3", "R2", "R1"};

  const auto r1 = cat.check_eligibility(ev, 1, 4, classes);
  CHECK(!r1.eligible && r1.reason.find("MID-ENGINE") != std::string::npos);
  const auto r2 = cat.check_eligibility(ev, 2, 4, classes);
  CHECK(!r2.eligible && r2.reason.find("8-CYLINDER") != std::string::npos);
  const auto r3 = cat.check_eligibility(ev, 3, 4, classes);
  CHECK(r3.eligible);
}

}  // namespace

int main() {
  pre_race();
  post_race();
  entry();
  timed();
  disc_ticket_strings();
  nemesis_grid_placement();
  missed_checkpoint_hairpin_and_fade();
  career_restriction_engine_placement_and_cylinders();
  if (failures) {
    std::printf("race_flow_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("race_flow_tests: ok\n");
  return 0;
}
