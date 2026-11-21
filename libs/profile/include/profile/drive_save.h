// The imported profile as the engine's own save (FORMAT-PROFILE, Import
// columns): the career, the collectibles and the owned cars.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "drive/save.h"
#include "garage/garage.h"
#include "profile/profile.h"

namespace profile {

// Fills `out` from `p`:
// - car: the CareerCar garage row's model through `media_of` (a Data_Car id
//   to its MediaName, "" when the engine cannot load it); when that model is
//   missing, the next owned car it can load (a missing model is kept but
//   hidden);
// - the career: Credits, TotalWinnings, XP (the wristband points), the race
//   counters, the TimeIn*Races times, TopSpeed, the best place per race
//   and event;
// - free roam: the found signs, the barns off their starting state with
//   their distances, TotalDistanceDriven;
// - garage: every garage row's model, in Id order.
// There is no position: the import spawns at the festival (Player0's
// layout is open). `notes` receives what was checked or left out (the
// UpgradeDiscount check, hidden cars).
void to_drive_save(const Imported& p, const std::function<std::string(int64_t)>& media_of, drive::DriveSave& out,
                   std::vector<std::string>* notes);

// The garage (SPEC-GARAGE-UPGRADES, The owned-car record): every
// Career_Garage row with its stored index, class, parts value, paint and
// fitted slots; every Career_PurchasedParts row; LastUsedGarageId; the
// selected car (CareerCar, or the lowest Id).
garage::Garage to_garage(const Imported& p);

}  // namespace profile
