// Mods (ours, M7): folders of the player's own files laid over the play
// folder's bake and sound, without touching either.
//
// A mod is a folder <mods root>/<name>/ holding a mod.txt and any of
//   bake/cars/<car>.shiftbake, bake/cars/<car>.physics.txt,
//   bake/wheels/<wheel>.shiftbake, bake/shared.shiftbake  (carbake's files)
//   sound/<car>/                                          (soundbake's folder, whole)
// A file a mod holds replaces the play folder's file of the same relative
// path; a sound folder replaces the car's whole folder. mod.txt lines:
// "title <text>", "enabled 0|1" (default 1), "priority N" (default 0; the
// highest wins, then the name). A folder whose name starts with '_' or '.'
// is skipped. Mods replace files only: a car needs its database row too, so
// a mod cannot add one.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cars::mods {

struct Mod {
  std::string name, title;
  std::filesystem::path dir;
  bool enabled = true;
  int priority = 0;
  int files = 0;  // files under bake/ and sound/
};

// The mods under `root`, the winner first (priority, then name). Missing
// root: none.
std::vector<Mod> list(const std::filesystem::path& root);

// Lays the enabled mods over `bake_dir`, `sound_dir`, and (when given)
// `game_root`'s `media/` and `media/audio/radio/` for resolve(). Not
// thread-safe against resolve(): call once at start-up.
void activate(const std::vector<Mod>& mods, const std::filesystem::path& bake_dir,
              const std::filesystem::path& sound_dir, const std::filesystem::path& game_root = {});

// `p` itself, or the first enabled mod's file (or folder) at the same path
// relative to the bake or sound folder.
std::filesystem::path resolve(const std::filesystem::path& p);

// The active mods' names, the winner first.
const std::vector<std::string>& active();

}  // namespace cars::mods
