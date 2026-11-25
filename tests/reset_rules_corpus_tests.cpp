// Corpus: the reset tunables read from the disc's
// media/gametunablesettings.zip (GameTunableSettings.ini [HandOfGodReset]
// and AIRacing.xml ResetParameters in every settings set). Needs
// SHIFT_GAME_ROOT; exits 77 without it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "race/difficulty.h"
#include "race/reset_rules.h"

namespace {

int failures = 0;

#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      ++failures;                                                 \
    }                                                             \
  } while (0)

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

int main() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("reset_rules_corpus_tests: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  std::string error;
  race::HandOfGodReset hog;
  CHECK(race::load_hand_of_god_reset(root, hog, &error));
  if (!error.empty()) std::printf("HandOfGodReset: %s\n", error.c_str());
  // Every key came from the file (the fallbacks hold the same numbers, so
  // the values alone would not prove the read).
  for (const auto& k : hog.missing) std::printf("missing %s\n", k.c_str());
  CHECK(hog.missing.empty());
  std::printf("HandOfGodReset: facing %.0f mph cos %.3f %.0f s; progress %.0f mph %.0f s; off %.0f m %.0f s, "
              "%.0f m %.0f s; non-collide %.1f s, optimal speed %.2f, fade out %.1f s, fade in %.1f s, "
              "collisions off %.1f s\n",
              hog.facing_speed_mph, hog.facing_cos, hog.facing_time, hog.progress_mph, hog.progress_time,
              hog.off_track_m0, hog.off_track_time0, hog.off_track_m1, hog.off_track_time1,
              hog.post_reset_non_collide, hog.optimal_speed_scale, hog.fade_out, hog.fade_in, hog.collisions_off);
  CHECK(near(hog.facing_speed_mph, 40) && near(hog.facing_cos, 0.342) && near(hog.facing_time, 3));
  CHECK(near(hog.progress_mph, 5) && near(hog.progress_cos, 1.0) && near(hog.progress_time, 3));
  CHECK(near(hog.off_track_m0, 5) && near(hog.off_track_time0, 6));
  CHECK(near(hog.off_track_m1, 15) && near(hog.off_track_time1, 3));
  CHECK(near(hog.post_reset_non_collide, 3) && near(hog.optimal_speed_scale, 0.5));
  CHECK(near(hog.fade_out, 2) && near(hog.fade_in, 1) && near(hog.collisions_off, 2));

  // The AI's rules: present and identical in the three sets.
  for (const char* set : {"default", "freeroam", "streetrace"}) {
    race::AiResetRules r;
    error.clear();
    const bool ok = race::load_ai_reset_rules(root, set, r, &error);
    if (!ok) std::printf("AIRacing.xml %s: %s\n", set, error.c_str());
    CHECK(ok);
    std::printf("AIRacing.xml %s: stuck within %d/%d/%d waypoints for %.0f/%.0f/%.0f s, within %.1f m for %.0f s, "
                "below %.1f m/s for %.0f s, upside down (up.y < %.2f) below %.1f m/s for %.0f s, fell below %.0f m, "
                "cancelled after %.0f m\n",
                set, r.stuck_waypoints[0].count, r.stuck_waypoints[1].count, r.stuck_waypoints[2].count,
                r.stuck_waypoints[0].min_time, r.stuck_waypoints[1].min_time, r.stuck_waypoints[2].min_time,
                r.stuck_distance_m, r.stuck_distance_time, r.slow_speed, r.slow_time, r.upside_down_up_y,
                r.upside_down_speed, r.upside_down_time, r.fell_low, r.cancel_moved_m);
    CHECK(r.stuck_waypoints[0].count == 1 && near(r.stuck_waypoints[0].min_time, 25));
    CHECK(r.stuck_waypoints[1].count == 4 && near(r.stuck_waypoints[1].min_time, 30));
    CHECK(r.stuck_waypoints[2].count == 8 && near(r.stuck_waypoints[2].min_time, 35));
    CHECK(near(r.stuck_distance_m, 2) && near(r.stuck_distance_time, 25));
    CHECK(near(r.slow_speed, 3) && near(r.slow_time, 25));
    CHECK(near(r.upside_down_speed, 3) && near(r.upside_down_time, 3) && near(r.upside_down_up_y, 0.3));
    CHECK(near(r.fell_low, -50) && near(r.cancel_moved_m, 20));
  }
  {
    // WORLD-05: the race boosts' registry values equal the struct's
    // initialisers (the disc's), so a missing file changes nothing.
    std::map<std::string, double> reg;
    error.clear();
    const bool ok = race::load_global_registry(root, reg, &error);
    if (!ok) std::printf("GlobalRegistry.xml: %s\n", error.c_str());
    CHECK(ok);
    const race::RaceBoostTunables d;
    const auto t = race::RaceBoostTunables::from(reg);
    std::printf("GlobalRegistry.xml race boosts: start %.2f..%.2f held %.0f..%.0f m, decay %.0f m, skill x%.2f, "
                "gap %.0f m/car, behind-player %.0f m/s -> %.2f, min %.2f; nemesis x%.2f +%.2f, catch-up +%.2f over "
                "%.0f..%.0f m, off %.0f m from the finish\n",
                t.start_torque_first, t.start_torque_last, t.keep_full_first, t.keep_full_last, t.decay, t.max_skill,
                t.gap_per_car, t.min_speed_behind_player, t.torque_behind_player, t.torque_minimum, t.nemesis_skill,
                t.nemesis_torque, t.catchup_torque, t.catchup_min, t.catchup_max, t.catchup_end);
    CHECK(reg.count("AI/Racing/RaceBehavior/RaceStartBoost/TorqueScaleBoostForFirstCar") == 1);
    CHECK(near(t.start_torque_first, d.start_torque_first) && near(t.start_torque_last, d.start_torque_last));
    CHECK(near(t.keep_full_first, d.keep_full_first) && near(t.keep_full_last, d.keep_full_last));
    CHECK(near(t.decay, d.decay) && near(t.max_skill, d.max_skill) && near(t.gap_per_car, d.gap_per_car));
    CHECK(near(t.min_speed_behind_player, d.min_speed_behind_player) &&
          near(t.torque_behind_player, d.torque_behind_player) && near(t.torque_minimum, d.torque_minimum));
    CHECK(near(t.nemesis_skill, d.nemesis_skill) && near(t.nemesis_torque, d.nemesis_torque));
    CHECK(near(t.catchup_torque, d.catchup_torque) && near(t.catchup_min, d.catchup_min) &&
          near(t.catchup_max, d.catchup_max) && near(t.catchup_end, d.catchup_end));
  }
  std::printf("reset_rules_corpus_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
