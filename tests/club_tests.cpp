// The 1000 Club evaluator (SPEC-V4-CONTENT, The 1000 Club) on challenges
// written in the files' grammar (ours, not copied): a held speed, a timed
// trip with a crash fail, a jump sequence, and one the engine does not
// measure yet. With SHIFT_STATE_ROOT, every package file parses.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "freeroam/club.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                  \
    }                                                              \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

const char* kLocs =
    "\xEF\xBB\xBF<NamedTransforms>\n"
    "  <NamedTransform name='here'><Transform pos.x='0' pos.y='0' pos.z='0' radius='10'/></NamedTransform>\n"
    "  <NamedTransform name='there'><Transform pos.x='1000' pos.y='0' pos.z='0' radius=\"20\"/>"
    "<Transform pos.x='0' pos.y='0' pos.z='900'/></NamedTransform>\n"
    "</NamedTransforms>\n";

const char* kFile = R"(<car_challenges xmlns:xs="pg">
  <!-- held speed -->
  <display description="IDS_A" title="IDS_TOP_SPEED"/>
  <group type="and">
    <pass_when><timer is="more_equal" value="10"/><hud progress="true"/></pass_when>
    <fail_when><speed is ="less_equal" value="90"/></fail_when>
  </group>
  <display description="IDS_B" title="IDS_CLEAN_ROAD_TRIP"/>
  <a_to_b start_loc="here" end_loc="there">
    <fail_when><timer is="more_equal" value="60"/></fail_when>
    <fail_when><collided/></fail_when>
    <hud activate="true"/>
  </a_to_b>
  <display description="IDS_C" title="IDS_LONG_JUMP"/>
  <group type="then">
    <pass_when><distance is="more_equal" value="15"/></pass_when>
    <pass_when><distance is="more_equal" value="30"/></pass_when>
    <fail_unless><in_air/></fail_unless>
  </group>
  <display description="IDS_D" title="IDS_NEAR_MISSES"/>
  <pass_when pass_count="5"><skills type="nearmiss"/></pass_when>
</car_challenges>)";

}  // namespace

int main() {
  using namespace freeroam::club;
  Locations locs;
  std::string error;
  CHECK(parse_named_transforms(kLocs, locs, &error));
  if (!error.empty()) std::printf("club: %s\n", error.c_str());
  CHECK(locs.size() == 2 && locs["there"].points.size() == 2 && near(locs["there"].radius[1], kDefaultRadius));
  CHECK(locs["there"].inside(1015, 5) && !locs["there"].inside(1030, 0) && locs["there"].inside(3, 895));

  std::vector<Challenge> cs;
  CHECK(parse_challenges(kFile, cs, &error));
  if (!error.empty()) std::printf("club: %s\n", error.c_str());
  CHECK(cs.size() == 4);
  if (cs.size() != 4) return 1;
  CHECK(cs[0].supported && cs[1].supported && cs[2].supported && !cs[3].supported && cs[3].unsupported == "skills");
  CHECK(cs[0].label() == "top speed");
  std::printf("club: %s: %s\n", cs[1].label().c_str(), cs[1].summary().c_str());
  const double dt = 1.0 / 120;

  // Held speed: 95 mph, a dip to 80 at 5 s restarts it; 10 s more passes.
  {
    Run run(cs[0], locs);
    Signal s;
    double passed_at = -1;
    for (int k = 0; k < 120 * 20 && passed_at < 0; ++k) {
      s.time = k * dt;
      s.speed_mph = std::fabs(s.time - 5) < 0.05 ? 80 : 95;
      if (run.step(s)) passed_at = s.time;
    }
    std::printf("club: held speed passed at %.2f s\n", passed_at);
    CHECK(passed_at > 15.0 && passed_at < 15.2);
  }
  // The trip: waits for A; a crash on the way fails it; a clean drive passes.
  {
    Run run(cs[1], locs);
    Signal s;
    bool passed = false;
    double x = -50;
    int crashes = 0;
    for (int k = 0; k < 120 * 200 && !passed; ++k) {
      s.time = k * dt;
      x += 40 * dt;  // 40 m/s along x
      if (x > 1100) x = -50;
      s.x = x;
      s.collided = crashes == 0 && x > 500 && x < 501;
      crashes += s.collided;
      passed = run.step(s);
    }
    CHECK(passed && run.failures() == 1 && crashes == 1);
  }
  // A slow trip fails on its timer.
  {
    Run run(cs[1], locs);
    Signal s;
    bool passed = false;
    for (int k = 0; k < 120 * 70 && !passed; ++k) {
      s.time = k * dt;
      s.x = std::min(1000.0, 15.0 * s.time);
      passed = run.step(s);
    }
    CHECK(!passed && run.failures() == 1);
  }
  // Parked in A: the trip never starts, so it never fails; leaving starts it.
  {
    Run run(cs[1], locs);
    Signal s;
    bool passed = false;
    for (int k = 0; k < 120 * 200 && !passed; ++k) {
      s.time = k * dt;
      s.x = s.time < 150 ? 2.0 : std::min(1000.0, 2.0 + 40 * (s.time - 150));
      passed = run.step(s);
      if (k == 120 * 100) CHECK(run.status(s) == "leave here to start" && run.failures() == 0);
    }
    CHECK(passed && run.failures() == 0);
  }
  // The jump: 20 m then landing restarts it; a 35 m jump passes both steps.
  {
    Run run(cs[2], locs);
    Signal s;
    auto jump = [&](double length, double from) {
      for (int k = 0;; ++k) {
        s.time = from + k * dt;
        s.in_air = k > 0 && k * dt * 25 < length;
        s.jump_m = s.in_air ? k * dt * 25 : 0;
        if (run.step(s)) return true;
        if (k > 0 && !s.in_air) return false;
      }
    };
    CHECK(!jump(20, 0));
    CHECK(run.failures() == 1);
    CHECK(jump(35, 10));
  }
  {
    Run run(cs[3], locs);
    CHECK(!run.step(Signal{}) && run.status(Signal{}) == "not measured yet (skills)");
  }

  // The package's files and v4's locations, when the state copy is there.
  if (const char* state = std::getenv("SHIFT_STATE_ROOT")) {
    ClubData club;
    error.clear();
    CHECK(load_club(state, club, &error));
    if (!error.empty()) std::printf("club: %s\n", error.c_str());
    std::printf("club: package %s, %zu cars, %zu challenges, %zu measured today, %zu locations\n",
                club.package.c_str(), club.cars.size(), club.challenges(), club.measured(), club.locations.size());
    CHECK(club.cars.size() == 218 && club.challenges() == 1145 && club.measured() == 237);
    CHECK(club.locations.size() == 30);
    // Every location a measured challenge names is there.
    size_t missing = 0;
    for (const auto& [id, list] : club.cars)
      for (const auto& c : list)
        if (c.supported && Run(c, club.locations).status(Signal{}).rfind("not measured", 0) == 0) ++missing;
    CHECK(missing == 0);
  }

  if (failures) return 1;
  std::printf("club_tests: all passed\n");
  return 0;
}
