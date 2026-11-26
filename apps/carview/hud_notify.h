// The kinds of HUD notification (947_HUD's CombinedNotification and
// PopularityUnlock) and the original's HUD toggles, kept apart from
// hud_screen.h so the settings, the session and the HUD state can name them
// without the scene code.
#pragma once

#include <cstdint>

namespace carview {

// What a notification announces. Each retail kind reads its lines from the
// game's string tables (notify_strings() in hud_screen.h); kEngine carries
// the engine's own words for the events the original has no notification
// for.
enum class NotifyKind : uint8_t {
  kEngine,
  kRoadsDriven,         // ROADS DRIVEN / {0}/{1}
  kDiscountSigns,       // {0}/{1} DISCOUNT SIGNS SMASHED / Total upgrades discount: {0}
  kBarnRumour,          // NEW BARN FIND RUMOR
  kBarnRestoring,       // {0} IS BEING RESTORED / DAK WILL CALL YOU WHEN IT'S READY
  kBarnReady,           // BARN FIND READY - {0}
  kOutpostDiscovered,   // HORIZON OUTPOST DISCOVERED / NEW FAST TRAVEL DESTINATION UNLOCKED
  kSpeedStuntDone,      // SPEED STUNT COMPLETED / {0} Cr
  kSkillStuntDone,      // SKILL STUNT COMPLETED / {0} Cr
  kPhotoShootDone,      // PHOTO SHOOT COMPLETED / {0} Cr
  kSpeedStuntFailed,    // SPEED STUNT / YOU FAILED TO SET A SPEED
  kSkillStuntFailed,    // PR STUNT / TRY AGAIN
  kPhotoShootFailed,    // PHOTO SHOOT / TRY AGAIN
  kShowcaseUnlocked,    // SHOWCASE EVENT UNLOCKED! / Show me! (PopularityUnlock)
  kSponsorRank,         // RANK {0} UNLOCKED! over {0} Cr and the challenge's name
  kWristbandAvailable,  // NEW WRISTBAND AT RACE CENTRAL
};

// How long a notification of `kind` stays up, from its SHOW to its HIDE.
// GlobalRegistry.xml's Notifications/DurationsInSeconds give the barn
// find being restored and ready 10 s (BarnFindDiscovered too, which no
// engine kind is known to be: the rumour keeps the fallback); the others
// have no tunable there and keep the engine's `fallback` (Q351).
constexpr double notify_seconds(NotifyKind kind, double fallback) {
  switch (kind) {
    case NotifyKind::kBarnRestoring:
    case NotifyKind::kBarnReady: return 10.0;
    default: return fallback;
  }
}

// The icon a notification shows (CombinedNotification/Icons).
enum class NotifyIcon : uint8_t { kMisc, kSponsor, kMail };

// The original's HUD options (ForzaProfile.sch Options/*, InGame
// IDS_HudOption_*) the engine honours, on by default as in the original:
// LAP TIME AND POSITION (Timers: the race's lap, place, times and
// standings), WRONG WAY WARNING (WrongWay), NAMES OVER CARS (NamesOverCars)
// and POPULARITY AND SKILLS (PopularitySkills: the skill chain and the
// popularity disc). TRACKED ACHIEVEMENT is the engine's own card (the
// original HUD has nothing in the top left), so it is off unless asked for.
struct HudToggles {
  bool timers = true;
  bool wrong_way = true;
  bool names_over_cars = true;
  bool popularity_skills = true;
  bool tracked_achievement = false;
  // BARN FIND RADAR: the engine's proximity pill inside a barn rumour (the
  // original marks a rumour on the maps only), off unless asked for.
  bool barn_radar = false;
  bool operator==(const HudToggles&) const = default;
};

}  // namespace carview
