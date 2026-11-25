// Corpus: the reset tunables read from the disc's
// media/gametunablesettings.zip (GameTunableSettings.ini [HandOfGodReset]
// and AIRacing.xml ResetParameters in every settings set). Needs
// SHIFT_GAME_ROOT; exits 77 without it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

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
  std::printf("reset_rules_corpus_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
