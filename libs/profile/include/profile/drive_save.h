// The imported profile as the engine's own save (FORMAT-PROFILE, Import
// columns): the career, the collectibles and the owned cars.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "drive/save.h"
#include "garage/garage.h"
#include "profile/profile.h"

namespace profile {

// Main/Level is the zero-based PlayerLevelDef.Id (a profile edited to level
// 11 at 100,000 XP stays 11, the row from 95,000); the engine's driver level
// (ProgressionTables::driver_level_for_xp, DriveSave::level) is one-based.
int engine_driver_level_from_profile(int64_t main_level);
int64_t profile_level_from_engine(int engine_level);

// Where an imported career resumes: the collected wristband is
// Main/WristbandLevel (zero-based, clamped to 0..6); a save still in the
// first-time career (stage 1 or 2, no band, points or results) resumes in
// the engine's opening (interim, PROG-05) with no wristband (-1).
struct ImportedCareerStage {
  bool intro_completed = true;
  std::string campaign_stage;  // "freeroam", "viper_drive" or "qualify"
  int collected_wristband = 0;
};
ImportedCareerStage imported_career_stage(const Imported& p);

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
// - progression: popularity points, the Fame tiers claimed and every other
//   sponsor's {count, tiers} (SponsorshipState, SPEC_QUESTIONS Q60), the
//   driver level (Main/Level + 1, kept as saved; there is no driver XP),
//   MaxSkillsScore / MaxSkillsCarOrdinal, the collected wristband
//   (Main/WristbandLevel) and the opening stage (imported_career_stage).
//   The campaign mirror (bosses, showcases, finale, heard dialogue) needs
//   the catalog and the Fame ladder: race::infer_campaign_state fills it.
// There is no position: the import spawns at the festival (Player0's
// layout is open). `notes` receives what was checked or left out (the
// UpgradeDiscount check, hidden cars).
void to_drive_save(const Imported& p, const std::function<std::string(int64_t)>& media_of, drive::DriveSave& out,
                   std::vector<std::string>* notes);

// The garage (SPEC-GARAGE-UPGRADES, The owned-car record): every
// Career_Garage row with its stored index, class, parts value, paint and
// fitted slots (preserving explicit -1 sentinel slots); every
// Career_PurchasedParts row; LastUsedGarageId; the selected car (CareerCar,
// or the lowest Id).
garage::Garage to_garage(const Imported& p);

// Options for exporting a retail-compatible plaintext ForzaProfile folder.
struct ExportOptions {
  // Optional existing plaintext export folder (`ForzaProfile.body` +
  // `PlayerDatabase.sqlite`) used as a base template so unknown Main/* fields,
  // option structs, V0..V6, tail blocks (Rally block 2, Player0, challenges,
  // dialogues, sponsorships) and untouched SQLite tables survive intact.
  std::filesystem::path template_folder;
  // Optional in-memory template (used when `template_folder` is empty).
  const Imported* base_profile = nullptr;
  // VersionFlags override (0 = base, 2 = v4, -1 = auto).
  int version_flags = -1;
};

// Builds or updates a retail-compatible `Imported` (including its `Body` field
// tree, Colorado child records, CAverageSpeedState speed zones, 8-byte-stride
// 1000 Club records, and PlayerDatabase rows) from `save` + `garage`.
// When `base_template` is non-null, unknown fields, tail blocks, trailing bytes,
// and untouched garage columns from `base_template` are preserved.
void from_drive_save(const drive::DriveSave& save, const garage::Garage& garage, Imported& out,
                     const Imported* base_template = nullptr);

// High-level clean-room export: converts `save` + `garage` into a retail-compatible
// plaintext ForzaProfile export folder (`ForzaProfile.body`, `PlayerDatabase.sqlite`,
// `export.json`) atomically, preserving any base template blocks/tables when provided.
bool export_forza_profile(const drive::DriveSave& save, const garage::Garage& garage,
                          const std::filesystem::path& out_folder, std::string* error,
                          const ExportOptions& opts = {});

}  // namespace profile
