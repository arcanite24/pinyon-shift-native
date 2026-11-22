// One-step save import (ours, Slice 13): a Pinyon Shift / Xenia save into
// the engine's own save (save.txt + garage.txt), from the CLI
// (profile-import --install) or the in-game PROFILE > IMPORT page.
//
// Accepted sources:
//  - a plaintext export folder (ForzaProfile.body + PlayerDatabase.sqlite,
//    FORMAT-PROFILE), found at the path or up to 6 folders below it (an
//    export root holding user/ and user-modded/ picks user/);
//  - a ZIP of such a folder (stored or deflated), extracted to a temporary
//    folder that is removed afterwards;
//  - a Xenia content folder or a Pinyon Shift state folder: shift-engine
//    carries no console key material, so the encrypted ForzaProfile there
//    is detected and refused with the export step to run first, unless a
//    plaintext export sits beside it.
//
// The import refuses to replace a save that already holds an imported
// career unless asked, snapshots the current save (before-import), keeps
// the first .pre-import copies of save.txt and garage.txt, and checks the
// build: a v4 save (version_flags 2) needs the v4 title update installed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "profile/backup.h"

namespace profile::import {

enum class SourceKind { kNone, kExportFolder, kExportZip, kEncryptedProfile };

struct Detection {
  SourceKind kind = SourceKind::kNone;
  std::filesystem::path export_dir;  // the folder holding ForzaProfile.body
  std::string description;
  std::string problem;  // why it cannot import ("" when it can)
};

// `work_dir` receives a ZIP's extraction.
Detection detect(const std::filesystem::path& src, const std::filesystem::path& work_dir);

// Extracts a ZIP (stored or deflated members) into `dir`.
bool extract_zip(const std::filesystem::path& zip, const std::filesystem::path& dir, std::string* error);

struct Options {
  std::filesystem::path save_dir;   // where save.txt and garage.txt go
  bool replace_existing = false;    // replace a save that holds an imported career
  bool v4_available = false;        // the v4 title update is installed
  backup::Config backups;           // before-import snapshot (backups_dir empty: none)
  // Data_Car id -> baked media name ("" when the engine cannot load it).
  std::function<std::string(int64_t)> media_of;
};

struct Result {
  bool ok = false;
  std::string error;
  Detection detection;
  bool v4_save = false;
  int64_t credits = 0;
  size_t garage_cars = 0, signs = 0;
  std::string car;
  std::vector<std::string> notes;
};

Result run(const std::filesystem::path& src, const Options& opts);

// Where the free roam starts (COLLISION.md's measured spawn, as
// world::kSpawn*): a first import has no position of its own.
inline constexpr double kFestivalSpawn[4] = {2002.96, 143.84, 960.00, 2.2008};

}  // namespace profile::import
