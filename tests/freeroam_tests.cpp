// Free-roam collectibles (SPEC-FREEROAM-COLLECTIBLES): the spec's synthetic
// cases for the map projection and the upgrade discount, the step rules on
// a small made-up set, and, with SHIFT_GAME_ROOT, the counts and
// placements read from the player's disc.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "freeroam/collectibles.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

const char* kFlyers = R"(<ActivityManager>
  <Activity type="ActivityFlyers" name="flyer_001" />
  <Activity type="ActivityFlyers" name="flyer_002" />
</ActivityManager>)";

const char* kBarns = R"(<ActivityManager>
  <!-- <Activity type="ActivityBarnFind" name="barnfind_template"><TriggerZone object="NOPE"/></Activity> -->
  <Activity type="ActivityBarnFindController" name="barnfind_controller" />
  <Activity type="ActivityBarnFind" name="barnfind_01">
    <HintRegion x_offset="-20" y_offset="30" angle="0" x_radius="400" y_radius="400" />
    <DoorInfo open_id="BF_A_OPEN" closed_id="BF_A_CLOSED" open_coll_id="BF_A_OPENC" closed_coll_id="BF_A_CLOSEDC" />
    <TriggerZone object="BARNFIND_A" name="BARNFIND_A" radius="10" maxMPH="50" />
    <UnlockCar id="9001" stringTableNameId="IDS_Barnfind_Car_A"/>
  </Activity>
  <Activity type="ActivityBarnFind" name="barnfind_02">
    <HintRegion x_offset="0" y_offset="0" angle="0" x_radius="300" y_radius="300" />
    <DoorInfo open_id="BF_B_OPEN" closed_id="BF_B_CLOSED" open_coll_id="BF_B_OPENC" closed_coll_id="BF_B_CLOSEDC" />
    <TriggerZone object="BARNFIND_B" name="BARNFIND_B" radius="10" maxMPH="50" />
    <UnlockCar id="9002" stringTableNameId="IDS_Barnfind_Car_B"/>
  </Activity>
</ActivityManager>)";

const char* kObjs = R"(<?xml version="1.0" ?>
<GameObjs>
  <Obj0 GameplayID="flyer_001"><Pos x="100" y="10" z="200"/></Obj0>
  <Obj1 GameplayID="flyer_002"><Pos x="-50" y="0" z="0"/></Obj1>
  <Obj2 GameplayID="BARNFIND_A"><Pos x="1000" y="5" z="1000"/></Obj2>
  <Obj3 GameplayID="BARNFIND_B"><Pos x="-1000" y="5" z="-1000"/></Obj3>
</GameObjs>)";

}  // namespace

int main() {
  using namespace freeroam;

  // The projection's corners and centre.
  auto uv = map_uv(-9583.3, 4600);
  CHECK(near(uv[0], 0) && near(uv[1], 0));
  uv = map_uv(9583.3, -6900);
  CHECK(near(uv[0], 1, 1e-5) && near(uv[1], 1));
  uv = map_uv(0, -1150);
  CHECK(near(uv[0], 0.5, 1e-5) && near(uv[1], 0.5));
  // The reference capture's player: u 0.72160, v 0.34120.
  uv = map_uv(4247.4, 676.2);
  CHECK(near(uv[0], 0.72160, 5e-5) && near(uv[1], 0.34120, 5e-5));

  // The discount.
  CHECK(discounted_price(10000, 85) == 1500);
  CHECK(discounted_price(10000, 0) == 10000);
  CHECK(discounted_price(10000, 100) == 0);
  CHECK(discounted_price(999, 1) == 989);
  CHECK(discounted_price(50, 1) == 50);
  CHECK(discounted_price(6200, 85) == 930);
  CHECK(upgrade_discount(85) == 85 && upgrade_discount(140) == 100);

  // The step rules on a made-up set.
  Collectibles data;
  std::string error;
  CHECK(parse_collectibles(kFlyers, kBarns, kObjs, data, &error));
  CHECK(data.signs.size() == 2 && data.barns.size() == 2);
  if (data.barns.size() == 2) {
    const Barn& a = data.barns[0];
    CHECK(a.name == "barnfind_01" && a.trigger == "barnfind_a" && a.car_id == 9001);
    CHECK(near(a.hint_x, 980) && near(a.hint_z, 1030) && near(a.hint_rx, 400));
    CHECK(a.open_id == "bf_a_open" && a.closed_coll_id == "bf_a_closedc");
  }
  Progress progress;
  FreeRoam fr(data, progress);
  CHECK(fr.hidden()->count("bf_a_open") && fr.hidden()->count("bf_a_openc") && !fr.hidden()->count("bf_a_closed"));
  CHECK(!fr.hidden()->count("flyer_001"));

  CarState car;
  car.x = 100, car.y = 10.5, car.z = 198.0;  // the sign 2 m ahead, inside the body's 2.2 m half length
  car.speed = 0.5;
  CHECK(fr.step(car).empty());  // too slow to break it
  car.speed = 10;
  auto ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kSignSmashed && ev[0].index == 0);
  CHECK(fr.signs_found() == 1 && fr.hidden()->count("flyer_001"));
  CHECK(fr.step(car).empty());  // once only
  car.x = 103;  // 3 m to the side: outside the 0.9 + 0.6 m half width
  car.z = 200;
  CHECK(fr.step(car).empty());

  // No rumour before 50 km; then barn 01, then barn 02 50 km later.
  car.x = 5000, car.z = 5000, car.moved = 49999;
  CHECK(fr.step(car).empty());
  car.moved = 1;
  ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kRumourStarted && ev[0].index == 0);
  CHECK(progress.barns[0].status == kBarnRumour && near(progress.barns[0].rumour_at, 50000));
  car.moved = 0;
  // Discovery: within 10 m and below 50 mph (22.35 m/s).
  car.x = 1005, car.z = 1000, car.speed = 25;
  CHECK(fr.step(car).empty());
  car.speed = 20;
  ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kBarnFound && ev[0].index == 0);
  CHECK(progress.barns[0].status == kBarnFound && progress.garage.size() == 1 && progress.garage[0] == 9001);
  CHECK(fr.hidden()->count("bf_a_closed") && fr.hidden()->count("barnfind_a") && !fr.hidden()->count("bf_a_open"));
  // Barn 02 is not spawned: driving to it finds nothing.
  car.x = -1000, car.z = -1000, car.speed = 5;
  CHECK(fr.step(car).empty() && progress.barns[1].status == kBarnNotSpawned);
  car.moved = 50000;
  ev = fr.step(car);  // the rumour starts this step; discovery tests next step
  CHECK(ev.size() == 1 && ev[0].kind == Event::kRumourStarted && ev[0].index == 1);
  car.moved = 0;
  ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kBarnFound && ev[0].index == 1 && fr.barns_found() == 2);

  // A sign that is not placed is an error.
  Collectibles bad;
  CHECK(!parse_collectibles(R"(<A><Activity type="ActivityFlyers" name="flyer_404"/></A>)", "<A/>", kObjs, bad,
                            &error));

  // The player's disc.
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) {
    Collectibles disc;
    error.clear();
    CHECK(load_collectibles(root, disc, &error));
    if (!error.empty()) std::printf("%s\n", error.c_str());
    std::printf("disc: %zu signs, %zu barn finds\n", disc.signs.size(), disc.barns.size());
    CHECK(disc.signs.size() == 100 && disc.barns.size() == 9);
    if (!disc.signs.empty()) {
      const auto& s = disc.signs[0];
      CHECK(s.id == "flyer_001" && near(s.at.x, 1410.35, 0.01) && near(s.at.z, 3126.53, 0.01));
    }
    if (disc.barns.size() == 9) {
      const int cars[9] = {1219, 1040, 1277, 1105, 1386, 1154, 1301, 251, 1155};
      std::set<std::string> doors;
      for (int i = 0; i < 9; ++i) {
        const Barn& b = disc.barns[size_t(i)];
        CHECK(b.car_id == cars[i] && near(b.radius, 10) && near(b.max_mph, 50));
        CHECK(b.hint_rx >= 286 && b.hint_rx <= 600 && near(b.hint_rx, b.hint_rz));
        doors.insert({b.open_id, b.closed_id, b.open_coll_id, b.closed_coll_id});
      }
      CHECK(doors.size() == 36);
      CHECK(disc.barns[0].trigger == "barnfind_bug_eb110bf_92" && near(disc.barns[0].at.x, 1423.378, 0.01));
    }
  } else {
    std::printf("SHIFT_GAME_ROOT not set: the disc checks skipped\n");
  }

  if (failures) return 1;
  std::printf("freeroam_tests: all passed\n");
  return 0;
}
