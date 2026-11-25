// The game's reset tunables (SPEC-EVENTS, Wrong way, resets and leaving
// the track; SPEC-RACE-AI, Contacts and resets), read from
// media/gametunablesettings.zip at run time:
// - GameTunableSettings.ini [HandOfGodReset]: the automatic (player)
//   reset's triggers and the reset's fade and collision timings;
// - AIRacing.xml <Settings name="..."><Batch><MetaAIDriver><ResetParameters>:
//   the AI's own reset rules, one block per settings set (default,
//   freeroam, streetrace).
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace race {

// GameTunableSettings.ini [HandOfGodReset]. The initialisers are the
// fallbacks used only when a key is missing from the file (they are the
// file's own values); `missing` lists the keys the file did not give.
struct HandOfGodReset {
  double facing_speed_mph = 40;      // FacingSpeedThresholdMPH
  double facing_cos = 0.342;         // FacingCosAngle
  double facing_time = 3;            // FacingTimeBeforeReset (s)
  double progress_mph = 5;           // ProgressThresholdMPH
  double progress_cos = 1.0;         // ProgressCosAngle (read, not used: its meaning is not traced)
  double progress_time = 3;          // ProgressTimeBeforeReset (s)
  double off_track_m0 = 5;           // OffTrackAmountMeters0
  double off_track_time0 = 6;        // OffTrackTimeBeforeReset0 (s)
  double off_track_m1 = 15;          // OffTrackAmountMeters1
  double off_track_time1 = 3;        // OffTrackTimeBeforeReset1 (s)
  double post_reset_non_collide = 3; // PostResetNonCollideTime (s)
  double optimal_speed_scale = 0.5;  // OptimalSpeedScale
  double fade_out = 2;               // FadeOutTime (s)
  double fade_in = 1;                // FadeInTime (s)
  double collisions_off = 2;         // CollisionsOffTime (s)
  std::vector<std::string> missing;  // "HandOfGodReset\\<key>" not in the file
};

// The AI's ResetParameters (every attribute required; no fallbacks).
struct AiResetRules {
  struct Waypoints {
    int count = 0;        // ResetIfStuckWithinN<k>Waypoints_NumWaypoints
    double min_time = 0;  // ResetIfStuckWithinN<k>Waypoints_MinTime (s)
  };
  std::array<Waypoints, 3> stuck_waypoints{};
  double stuck_distance_m = 0;     // ResetIfStuckWithinThisDistance_Distance_m
  double stuck_distance_time = 0;  // ResetIfStuckWithinThisDistance_MinTime (s)
  double slow_speed = 0;           // ResetIfGoingThisSlow_Speed_mpers (m/s)
  double slow_time = 0;            // ResetIfGoingThisSlow_MinTime (s)
  double upside_down_speed = 0;    // ResetIfGoingThisSlowUpsideDown_Speed_mpers (m/s)
  double upside_down_time = 0;     // ResetIfGoingThisSlowUpsideDown_MinTime (s)
  double upside_down_up_y = 0;     // ResetIfGoingThisSlowUpsideDown_UpVectorYRolloverThreshold
  double fell_low = 0;             // ResetIfFellThisLow (m, world height)
  double cancel_moved_m = 0;       // CancelResetIfMovedThisFar_m
};

// Fills the keys the ini text gives; the rest keep their fallbacks and
// are listed in `missing`. False only when the text has no
// [HandOfGodReset] key at all.
bool parse_hand_of_god_reset(std::string_view ini_text, HandOfGodReset& out, std::string* error);
// The named set's ResetParameters; false (with `error`) when the set, the
// block or any attribute is missing.
bool parse_ai_reset_rules(std::string_view airacing_xml, std::string_view set, AiResetRules& out, std::string* error);

// Both from <game_root>/media/gametunablesettings.zip (Title Update v4
// carries neither member).
bool load_hand_of_god_reset(const std::filesystem::path& game_root, HandOfGodReset& out, std::string* error);
bool load_ai_reset_rules(const std::filesystem::path& game_root, std::string_view set, AiResetRules& out,
                         std::string* error);

// The AIRacing.xml set a race's AI runs with: "streetrace" for street and
// other traffic races (SPEC-RACE-AI, Routes with no AI line), "freeroam"
// in free roam, else "default" (interim: the selector is not decoded).
const char* ai_settings_set(bool street_race, bool free_roam = false);

}  // namespace race
