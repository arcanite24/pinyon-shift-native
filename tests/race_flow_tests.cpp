// The race-flow model (apps/carview/race_flow.h): the original's screen
// orders around a race, per race kind, and the timed advances.
#include <cstdio>
#include <vector>

#include "race_flow.h"

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
}

}  // namespace

int main() {
  pre_race();
  post_race();
  entry();
  timed();
  if (failures) {
    std::printf("race_flow_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("race_flow_tests: ok\n");
  return 0;
}
