// The kinds of HUD notification (947_HUD's CombinedNotification and
// PopularityUnlock), kept apart from hud_screen.h so the session and the HUD
// state can name them without the scene code.
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

// The icon a notification shows (CombinedNotification/Icons).
enum class NotifyIcon : uint8_t { kMisc, kSponsor, kMail };

}  // namespace carview
